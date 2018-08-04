#include "boost/algorithm/string.hpp"

#include "caffe/caffe.hpp"
#include "ristretto/quantization.hpp"


using caffe::Caffe;
using caffe::Net;
using caffe::string;
using caffe::vector;
using caffe::Blob;
using caffe::LayerParameter;
using caffe::NetParameter;

Quantization::Quantization(string model, string weights, string model_quantized,
      int iterations, string trimming_mode, int bitwidth_weights, int bitwidth_activations, string gpus) {
  this->model_ = model;
  this->weights_ = weights;
  this->model_quantized_ = model_quantized;
  this->iterations_ = iterations;
  this->trimming_mode_ = trimming_mode;
  this->bitwidth_weights_ = bitwidth_weights;
  this->bitwidth_activations_ = bitwidth_activations;
  this->gpus_ = gpus;

  // Could possibly improve choice of exponent. Experiments show LeNet needs
  // 4bits, but the saturation border is at 3bits (when assuming infinitely long
  // mantisssa).
  this->exp_bits_ = 4;
}

void Quantization::QuantizeNet() {
  CheckWritePermissions(model_quantized_);
  SetGpu();
  // Run the reference floating point network on validation set to find baseline
  // accuracy.
  Net<float>* net_val = new Net<float>(model_, caffe::TEST);
  net_val->CopyTrainedLayersFrom(weights_);
  float accuracy;
  // Run the reference floating point network on train data set to find maximum
  // values.
  RunForwardBatches(this->iterations_, net_val, &accuracy, true);
  test_score_baseline_ = accuracy;
  delete net_val;
  // Do network quantization and scoring.
  if (trimming_mode_ == "dynamic_fixed_point") {
    Quantize2DynamicFixedPoint();
  } else {
    LOG(FATAL) << "Unknown trimming mode: " << trimming_mode_;
  }
}

void Quantization::CheckWritePermissions(const string path) {
  std::ofstream probe_ofs(path.c_str());
  if (probe_ofs.good()) {
    probe_ofs.close();
    std::remove(path.c_str());
  } else {
    LOG(FATAL) << "Missing write permissions";
  }
}

void Quantization::SetGpu() {
  // Parse GPU ids or use all available devices
  vector<int> gpus;
  if (gpus_ == "all") {
    int count = 0;
#ifndef CPU_ONLY
    CUDA_CHECK(cudaGetDeviceCount(&count));
#else
    NO_GPU;
#endif
    for (int i = 0; i < count; ++i) {
      gpus.push_back(i);
    }
  } else if (gpus_.size()) {
    vector<string> strings;
    boost::split(strings, gpus_, boost::is_any_of(","));
    for (int i = 0; i < strings.size(); ++i) {
      gpus.push_back(boost::lexical_cast<int>(strings[i]));
    }
  } else {
    CHECK_EQ(gpus.size(), 0);
  }
  // Set device id and mode
  if (gpus.size() != 0) {
    LOG(INFO) << "Use GPU with device ID " << gpus[0];
    Caffe::SetDevice(gpus[0]);
    Caffe::set_mode(Caffe::GPU);
  } else {
    LOG(INFO) << "Use CPU.";
    Caffe::set_mode(Caffe::CPU);
  }
}

void Quantization::RunForwardBatches(const int iterations,
      Net<float>* caffe_net, float* accuracy, const bool do_stats,
      const int score_number) {
  LOG(INFO) << "Running for " << iterations << " iterations.";
  vector<Blob<float>* > bottom_vec;
  vector<int> test_score_output_id;
  vector<float> test_score;
  float loss = 0.0;
  for (int i = 0; i < iterations; ++i) {
    float iter_loss;
    // Do forward propagation.
    const vector<Blob<float>*>& result =
        caffe_net->Forward(bottom_vec, &iter_loss);
    // Find maximal values in network.
    if(do_stats) {
      caffe_net->RangeInLayers(&layer_names_, &max_in_, &max_out_,
          &max_params_);
    }
    // Keep track of network score over multiple batches.
    loss += iter_loss;
    int idx = 0;
    for (int j = 0; j < result.size(); ++j) {
      const float* result_vec = result[j]->cpu_data();
      for (int k = 0; k < result[j]->count(); ++k, ++idx) {
        const float score = result_vec[k];
        if (i == 0) {
          test_score.push_back(score);
          test_score_output_id.push_back(j);
        } else {
          test_score[idx] += score;
        }
        const std::string& output_name = caffe_net->blob_names()[
            caffe_net->output_blob_indices()[j]];
        LOG(INFO) << "Batch " << i << ", " << output_name << " = " << score;
      }
    }
  }
  loss /= iterations;
  LOG(INFO) << "Loss: " << loss;
  for (int i = 0; i < test_score.size(); ++i) {
    const std::string& output_name = caffe_net->blob_names()[
        caffe_net->output_blob_indices()[test_score_output_id[i]]];
    const float loss_weight = caffe_net->blob_loss_weights()[
        caffe_net->output_blob_indices()[test_score_output_id[i]]];
    std::ostringstream loss_msg_stream;
    const float mean_score = test_score[i] / iterations;
    if (loss_weight) {
      loss_msg_stream << " (* " << loss_weight
                      << " = " << loss_weight * mean_score << " loss)";
    }
    LOG(INFO) << output_name << " = " << mean_score << loss_msg_stream.str();
  }
  *accuracy = test_score[score_number] / iterations;
}

void Quantization::Quantize2DynamicFixedPoint() {
  // Find the integer length for dynamic fixed point numbers.
  // The integer length is chosen such that no saturation occurs.
  // This approximation assumes an infinitely long fractional part.
  // For layer activations, we reduce the integer length by one bit.
  for (int i = 0; i < layer_names_.size(); ++i) {
    il_in_.push_back((int)ceil(log2(max_in_[i]+1e-8+1))); // ??? +1 due to signed int type
    il_out_.push_back((int)ceil(log2(max_out_[i]+1e-8+1))); // ??? +1 due to signed int type
    il_params_.push_back((int)ceil(log2(max_params_[i])+1e-8+1)); // ??? +1 due to signed int type
  }
  // Debug
  for (int k = 0; k < layer_names_.size(); ++k) {
    LOG(INFO) << "Layer " << layer_names_[k] << ", integer length input=" << il_in_[k] << ", integer length output=" << il_out_[k] << ", integer length parameters=" << il_params_[k];
  }

  // Score net with dynamic fixed point convolution parameters.
  // The rest of the net remains in high precision format.
  NetParameter param;
  caffe::ReadNetParamsFromTextFileOrDie(model_, &param);
  param.mutable_state()->set_phase(caffe::TEST);
  vector<int> test_bw_conv_params;
  vector<float> test_scores_conv_params;
  float accuracy;
  Net<float>* net_test;
  int bitwidth;
  bitwidth = this->bitwidth_weights_;
  // weights
  EditNetDescriptionDynamicFixedPoint(&param, "Convolution", "Parameters",
      bitwidth, -1, -1, -1);
  net_test = new Net<float>(param, NULL);
  net_test->CopyTrainedLayersFrom(weights_);
  RunForwardBatches(iterations_, net_test, &accuracy);
  test_bw_conv_params.push_back(bitwidth);
  test_scores_conv_params.push_back(accuracy);
  delete net_test;
  // Score net with dynamic fixed point inner product parameters.
  // The rest of the net remains in high precision format.
  caffe::ReadNetParamsFromTextFileOrDie(model_, &param);
  param.mutable_state()->set_phase(caffe::TEST);
  vector<int> test_bw_fc_params;
  vector<float> test_scores_fc_params;
  // weights
  EditNetDescriptionDynamicFixedPoint(&param, "InnerProduct", "Parameters",
      -1, bitwidth, -1, -1);
  net_test = new Net<float>(param, NULL);
  net_test->CopyTrainedLayersFrom(weights_);
  RunForwardBatches(iterations_, net_test, &accuracy);
  test_bw_fc_params.push_back(bitwidth);
  test_scores_fc_params.push_back(accuracy);
  delete net_test;
  // Score net with dynamic fixed point layer activations.
  // The rest of the net remains in high precision format.
  bitwidth = this->bitwidth_activations_;
  caffe::ReadNetParamsFromTextFileOrDie(model_, &param);
  param.mutable_state()->set_phase(caffe::TEST);
  vector<int> test_bw_layer_activations;
  vector<float> test_scores_layer_activations;
  // activations
  EditNetDescriptionDynamicFixedPoint(&param, "Convolution_and_InnerProduct",
      "Activations", -1, -1, bitwidth, bitwidth);
  net_test = new Net<float>(param, NULL);
  net_test->CopyTrainedLayersFrom(weights_);
  RunForwardBatches(iterations_, net_test, &accuracy);
  test_bw_layer_activations.push_back(bitwidth);
  test_scores_layer_activations.push_back(accuracy);
  delete net_test;
  // Choose bit-width for different network parts
  bw_conv_params_ = test_bw_conv_params[0];
  bw_fc_params_ = test_bw_fc_params[0];
  bw_out_ = test_bw_layer_activations[0];
  bw_in_ = bw_out_;
  // Score dynamic fixed point network.
  // This network combines dynamic fixed point parameters in convolutional and
  // inner product layers, as well as dynamic fixed point activations.
  caffe::ReadNetParamsFromTextFileOrDie(model_, &param);
  param.mutable_state()->set_phase(caffe::TEST);
  EditNetDescriptionDynamicFixedPoint(&param, "Convolution_and_InnerProduct",
      "Parameters_and_Activations", bw_conv_params_, bw_fc_params_, bw_in_,
      bw_out_);
  net_test = new Net<float>(param, NULL);
  net_test->CopyTrainedLayersFrom(weights_);
  RunForwardBatches(iterations_, net_test, &accuracy);
  delete net_test;
  param.release_state();
  WriteProtoToTextFile(param, model_quantized_);
  // Write summary of dynamic fixed point analysis to log
  LOG(INFO) << "------------------------------";
  LOG(INFO) << "Network accuracy analysis for convolutional (CONV) and fully connected (FC) layers.";
  LOG(INFO) << "Baseline 32-bit float: " << test_score_baseline_;
  LOG(INFO) << "Dynamic fixed-point CONV weights:";
  for (int j = 0; j < test_scores_conv_params.size(); ++j) {
    LOG(INFO) << test_bw_conv_params[j] << "-bit: \t" << test_scores_conv_params[j];
  }
  LOG(INFO) << "Dynamic fixed-point FC weights:";
  for (int j = 0; j < test_scores_fc_params.size(); ++j) {
    LOG(INFO) << test_bw_fc_params[j] << "-bit: \t" << test_scores_fc_params[j];
  }
  LOG(INFO) << "Dynamic fixed-point layer activations:";
  for (int j = 0; j < test_scores_layer_activations.size(); ++j) {
    LOG(INFO) << test_bw_layer_activations[j] << "-bit: \t" << test_scores_layer_activations[j];
  }
  LOG(INFO) << "Dynamic fixed-point net:";
  LOG(INFO) << bw_conv_params_ << "-bit CONV weights,";
  LOG(INFO) << bw_fc_params_ << "-bit FC weights,";
  LOG(INFO) << bw_out_ << "-bit layer activations:";
  LOG(INFO) << "Accuracy: " << accuracy;
  LOG(INFO) << "Please fine-tune.";
}

void Quantization::EditNetDescriptionDynamicFixedPoint(NetParameter* param,
      const string layers_2_quantize, const string net_part, const int bw_conv,
      const int bw_fc, const int bw_in, const int bw_out) {
  for (int i = 0; i < param->layer_size(); ++i) {
    // if this is a convolutional layer which should be quantized ...
    if (layers_2_quantize.find("Convolution") != string::npos &&
        param->layer(i).type().find("Convolution") != string::npos) {
      // quantize parameters
      if (net_part.find("Parameters") != string::npos) {
        LayerParameter* param_layer = param->mutable_layer(i);
        param_layer->set_type("ConvolutionRistretto");
        param_layer->mutable_quantization_param()->set_fl_params(bw_conv -
            GetIntegerLengthParams(param->layer(i).name()));
        param_layer->mutable_quantization_param()->set_bw_params(bw_conv);
      }
      // quantize activations
      if (net_part.find("Activations") != string::npos) {
        LayerParameter* param_layer = param->mutable_layer(i);
        param_layer->set_type("ConvolutionRistretto");
        param_layer->mutable_quantization_param()->set_fl_layer_in(bw_in -
            GetIntegerLengthIn(param->layer(i).name()));
        param_layer->mutable_quantization_param()->set_bw_layer_in(bw_in);
        param_layer->mutable_quantization_param()->set_fl_layer_out(bw_out -
            GetIntegerLengthOut(param->layer(i).name()));
        param_layer->mutable_quantization_param()->set_bw_layer_out(bw_out);
      }
    }
    // if this is an inner product layer which should be quantized ...
    if (layers_2_quantize.find("InnerProduct") != string::npos &&
        (param->layer(i).type().find("InnerProduct") != string::npos ||
        param->layer(i).type().find("FcRistretto") != string::npos)) {
      // quantize parameters
      if (net_part.find("Parameters") != string::npos) {
        LayerParameter* param_layer = param->mutable_layer(i);
        param_layer->set_type("FcRistretto");
        param_layer->mutable_quantization_param()->set_fl_params(bw_fc -
            GetIntegerLengthParams(param->layer(i).name()));
        param_layer->mutable_quantization_param()->set_bw_params(bw_fc);
      }
      // quantize activations
      if (net_part.find("Activations") != string::npos) {
        LayerParameter* param_layer = param->mutable_layer(i);
        param_layer->set_type("FcRistretto");
        param_layer->mutable_quantization_param()->set_fl_layer_in(bw_in -
            GetIntegerLengthIn(param->layer(i).name()) );
        param_layer->mutable_quantization_param()->set_bw_layer_in(bw_in);
        param_layer->mutable_quantization_param()->set_fl_layer_out(bw_out -
            GetIntegerLengthOut(param->layer(i).name()) );
        param_layer->mutable_quantization_param()->set_bw_layer_out(bw_out);
      }
    }
  }
}

int Quantization::GetIntegerLengthParams(const string layer_name) {
  int pos = find(layer_names_.begin(), layer_names_.end(), layer_name)
      - layer_names_.begin();
  return il_params_[pos];
}

int Quantization::GetIntegerLengthIn(const string layer_name) {
  int pos = find(layer_names_.begin(), layer_names_.end(), layer_name)
      - layer_names_.begin();
  return il_in_[pos];
}

int Quantization::GetIntegerLengthOut(const string layer_name) {
  int pos = find(layer_names_.begin(), layer_names_.end(), layer_name)
      - layer_names_.begin();
  return il_out_[pos];
}
