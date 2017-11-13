//
// Created by jiashuai on 17-9-21.
//

#include <thundersvm/kernel/smo_kernel.h>
#include <thundersvm/model/svmmodel.h>
#include <thundersvm/kernel/kernelmatrix_kernel.h>
#include <iomanip>
using std::ofstream;
using std::endl;
using std::setprecision;
using std::ifstream;
using std::stringstream;
using svm_kernel::sum_kernel_values;

const char *SvmParam::kernel_type_name[6] = {"linear", "polynomial", "rbf", "sigmoid", "precomputed", "NULL"};
const char *SvmParam::svm_type_name[6] = {"c_svc", "nu_svc", "one_class", "epsilon_svr", "nu_svr",
                                          "NULL"};  /* svm_type */

void SvmModel::model_setup(const DataSet &dataset, SvmParam &param) {
    n_binary_models = n_classes * (n_classes - 1) / 2;
    rho.resize(n_binary_models);
    n_sv.resize(n_classes);
    sv.clear();
    this->param = param;
}

vector<float_type> SvmModel::cross_validation(DataSet dataset, SvmParam param, int n_fold) {
    dataset.group_classes(this->param.svm_type == SvmParam::C_SVC);//group classes only for classification

    vector<float_type> y_test_all;
    vector<float_type> y_predict_all;

    for (int k = 0; k < n_fold; ++k) {
        LOG(INFO) << n_fold << " fold cross-validation(" << k + 1 << "/" << n_fold << ")";
        DataSet::node2d x_train, x_test;
        vector<float_type> y_train, y_test;
        for (int i = 0; i < dataset.n_classes(); ++i) {
            int fold_test_count = dataset.count()[i] / n_fold;
            vector<int> class_idx = dataset.original_index(i);
            auto idx_begin = class_idx.begin() + fold_test_count * k;
            auto idx_end = idx_begin;
            while (idx_end != class_idx.end() && idx_end - idx_begin < fold_test_count) idx_end++;
            for (int j: vector<int>(idx_begin, idx_end)) {
                x_test.push_back(dataset.instances()[j]);
                y_test.push_back(dataset.y()[j]);
            }
            class_idx.erase(idx_begin, idx_end);
            for (int j:class_idx) {
                x_train.push_back(dataset.instances()[j]);
                y_train.push_back(dataset.y()[j]);
            }
        }
        DataSet train_dataset(x_train, dataset.n_features(), y_train);
        this->train(train_dataset, param);
        vector<float_type> y_predict = this->predict(x_test, 1000);
        y_test_all.insert(y_test_all.end(), y_test.begin(), y_test.end());
        y_predict_all.insert(y_predict_all.end(), y_predict.begin(), y_predict.end());
    }
    vector<float_type> test_predict = y_test_all;
    test_predict.insert(test_predict.end(), y_predict_all.begin(), y_predict_all.end());
    return test_predict;
}


void
SvmModel::predict_dec_values(const DataSet::node2d &instances, SyncData<float_type> &dec_values, int batch_size) const {
    SyncData<int> sv_start(n_classes);//start position of SVs in each class
    sv_start[0] = 0;
    for (int i = 1; i < n_classes; ++i) {
        sv_start[i] = sv_start[i - 1] + n_sv[i - 1];
    }

    //compute kernel values
    KernelMatrix k_mat(sv, param);

    auto batch_start = instances.begin();
    auto batch_end = batch_start;
    vector<float_type> predict_y;
    while (batch_end != instances.end()) {
        while (batch_end != instances.end() && batch_end - batch_start < batch_size) {
            batch_end++;
        }

        DataSet::node2d batch_ins(batch_start, batch_end);//get a batch of instances
        SyncData<float_type> kernel_values(batch_ins.size() * sv.size());
        k_mat.get_rows(batch_ins, kernel_values);
        SyncData<float_type> batch_dec_values(batch_ins.size() * n_binary_models);
#ifdef USE_CUDA
        batch_dec_values.set_device_data(
                &dec_values.device_data()[(batch_start - instances.begin()) * n_binary_models]);
#else
        batch_dec_values.set_host_data(
                &dec_values.host_data()[(batch_start - instances.begin()) * n_binary_models]);
#endif
        //sum kernel values and get decision values
        sum_kernel_values(coef, sv.size(), sv_start, n_sv, rho, kernel_values, batch_dec_values, n_classes,
                          batch_ins.size());
	batch_start += batch_size;
    }
}

vector<float_type> SvmModel::predict(const DataSet::node2d &instances, int batch_size) {
    SyncData<float_type> dec_values(instances.size() * n_binary_models);
    predict_dec_values(instances, dec_values, batch_size);
    vector<float_type> dec_values_vec(dec_values.size());
    memcpy(dec_values_vec.data(), dec_values.host_data(), dec_values.mem_size());
    return dec_values_vec;
}


void SvmModel::save_to_file(string path) {
    ofstream fs_model;
    fs_model.open(path.c_str());
    CHECK(fs_model.is_open()) << "create file " << path << "failed";
    const SvmParam &param = this->param;
    fs_model << "svm_type " << SvmParam::svm_type_name[param.svm_type] << endl;
    fs_model << "kernel_type " << SvmParam::kernel_type_name[param.kernel_type] << endl;
    if (param.kernel_type == SvmParam::POLY)
        fs_model << "degree " << param.degree << endl;
    if (param.kernel_type == SvmParam::POLY
        || param.kernel_type == SvmParam::RBF
        || param.kernel_type == SvmParam::SIGMOID)
        fs_model << "gamma " << param.gamma << endl;
    if (param.kernel_type == SvmParam::POLY || param.kernel_type == SvmParam::SIGMOID)
        fs_model << "coef0 " << param.coef0 << endl;
    fs_model << "nr_class " << n_classes << endl;
    fs_model << "total_sv " << sv.size() << endl;
    fs_model << "rho ";
    for (int i = 0; i < n_binary_models; ++i) {
        fs_model << rho[i] << " ";
    }
    fs_model << endl;
    if (param.svm_type == SvmParam::NU_SVC || param.svm_type == SvmParam::C_SVC) {
        fs_model << "label ";
        for (int i = 0; i < n_classes; ++i) {
            fs_model << label[i] << " ";
        }
        fs_model<< endl;
        fs_model << "nr_sv ";
        for (int i = 0; i < n_classes; ++i) {
            fs_model << n_sv[i] << " ";
        }
        fs_model<< endl;
    }
    //todo save probA and probB
    fs_model << "SV " << endl;
    for (int i = 0; i < sv.size(); i++) {
        for (int j = 0; j < n_classes - 1; ++j) {
            fs_model << setprecision(16) << coef[j * sv.size() + i] << " ";
        }

        vector<DataSet::node> p = sv[i];
        int k = 0;
//        if (param.kernel_type == SvmParam::PRECOMPUTED)
//            fs_model << "0:" << p[k].value << " ";
//        else
        for (; k < p.size(); k++) {
            fs_model << p[k].index << ":" << setprecision(8) << p[k].value << " ";
        }
        fs_model << endl;
    }
    fs_model.close();
}

void SvmModel::load_from_file(string path) {
    ifstream ifs;
    ifs.open(path.c_str());
    CHECK(ifs.is_open()) << "file " << path << " not found";
    string feature;
    while (ifs >> feature) {
        if (feature == "svm_type") {
            string value;
            ifs >> value;
            for (int i = 0; i < 6; i++) {
                if (value == SvmParam::svm_type_name[i])
                    param.svm_type = static_cast<SvmParam::SVM_TYPE>(i);
            }
        } else if (feature == "kernel_type") {
            string value;
            ifs >> value;
            for (int i = 0; i < 6; i++) {
                if (feature == SvmParam::kernel_type_name[i])
                    param.kernel_type = static_cast<SvmParam::KERNEL_TYPE>(i);
            }
        } else if (feature == "degree") {
            ifs >> param.degree;
        } else if (feature == "nr_class") {
            ifs >> n_classes;
            n_binary_models = n_classes * (n_classes - 1) / 2;
            rho.resize(n_binary_models);
            n_sv.resize(n_classes);
        } else if (feature == "coef0") {
            ifs >> param.coef0;
        } else if (feature == "gamma") {
            ifs >> param.gamma;

        } else if (feature == "total_sv") {
            ifs >> n_total_sv;
        } else if (feature == "rho") {
            for (int i = 0; i < n_binary_models; ++i) {
                ifs >> rho[i];
            }
        } else if (feature == "label") {
            label = vector<int>(n_classes);
            for (int i = 0; i < n_classes; ++i) {
                ifs >> label[i];
            }
        } else if (feature == "nr_sv") {
            for (int i = 0; i < n_classes; ++i) {
                ifs >> n_sv[i];
            }
        } else if (feature == "SV") {
            sv.clear();
            coef.resize((n_classes - 1) * n_total_sv);
            string line;
            getline(ifs, line);
            for (int i = 0; i < n_total_sv; i++) {
                getline(ifs, line);
                stringstream ss(line);
                for (int j = 0; j < n_classes - 1; ++j) {
                    ss >> coef[j * n_total_sv + i];
                }
                sv.emplace_back();//reserve space for an instance
                string tuple;
                while (ss >> tuple) {
                    sv.back().emplace_back(0, 0);
                    CHECK_EQ(sscanf(tuple.c_str(), "%d:%f", &sv.back().back().index, &sv.back().back().value), 2)
                        << "error when loading model file";
                };
            }
            ifs.close();
        }
    }
    if (param.svm_type != SvmParam::C_SVC && param.svm_type != SvmParam::NU_SVC){
        n_sv[0] = n_total_sv;
        n_sv[1] = 0;
    }
}
