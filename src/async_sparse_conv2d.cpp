#include "async_sparse_conv2d.h"
#include <iostream>
#include <Eigen/Core>

#include <chrono>  // for high_resolution_clock


AsynSparseConvolution2D::AsynSparseConvolution2D(int dimension, int nIn, int nOut, int filter_size, bool first_layer, bool use_bias, bool debug) 
: dimension_(dimension), nIn_(nIn), nOut_(nOut), filter_size_(filter_size), first_layer_(first_layer), use_bias_(use_bias), 
  filter_size_tensor_(dimension), filter_volume_(std::pow(filter_size, dimension)), bias_(nOut), padding_(2*dimension), 
  kernel_indices_(filter_volume_, dimension_), weights_(filter_volume_, nIn*nOut), initialized_output_maps_(false), debug_(debug),
  initialized_input_maps_(false) 
{

    filter_size_tensor_ = Eigen::VectorXi::Constant(dimension, filter_size_);
    padding_ = Eigen::VectorXi::Constant(dimension * 2, filter_size_/2);
    
    for (int i=0; i<filter_volume_; i++)
    {
        kernel_indices_(i,1) = i % filter_size;
        kernel_indices_(i,0) = i / filter_size;
    }
}

AsynSparseConvolution2D::~AsynSparseConvolution2D()
{
    
}

void AsynSparseConvolution2D::setParameters(Eigen::VectorXf bias, Eigen::MatrixXf weights)
{
    bias_ = bias;
    weights_ = weights;
}

void AsynSparseConvolution2D::initMaps(int H, int W)    
{
    H_ = H;
    W_ = W;

    num_pixels_ = H*W;

    if (!initialized_input_maps_)
    {
        old_input_feature_map_ = Eigen::MatrixXf(num_pixels_, nIn_);
        old_input_feature_map_ = old_input_feature_map_.setConstant(0);
        initialized_input_maps_ = true;
    }

    if (!initialized_output_maps_)
    {
        output_feature_map_ = Eigen::MatrixXf(num_pixels_, nOut_);
        output_feature_map_ = output_feature_map_.setConstant(0);
        initialized_output_maps_ = true;
    }
}

AsynSparseConvolution2D::ActiveMatrix AsynSparseConvolution2D::initActiveMap(Eigen::MatrixXf input_feature_map, const Eigen::MatrixXi update_location)
{
    Eigen::VectorXi update_location_linear = update_location(Eigen::all,1) + W_ * update_location(Eigen::all,0);

    ActiveMatrix active_sites_map = ActiveMatrix::Constant(num_pixels_, 1, Site::INACTIVE);
    for (int i=0; i<input_feature_map.rows(); i++)
        if (input_feature_map(i,Eigen::all).array().abs().sum()>0)
            active_sites_map(i,0) = Site::ACTIVE;
            if (debug_)  std::cout << input_feature_map.rows() << std::endl;
    
    for (int i=0; i<update_location_linear.rows(); i++)
        active_sites_map(update_location_linear(i,0),0) = Site::ACTIVE;
    
    if (debug_)  std::cout << "init active sites" << std::endl;

    return active_sites_map;
}

AsynSparseConvolution2D::ReturnType AsynSparseConvolution2D::forward(const Eigen::Ref<const Eigen::MatrixXi> update_location,
                                                                     const Eigen::Ref<const Eigen::MatrixXf> input_feature_map, 
                                                                     Eigen::Ref<ActiveMatrix>& active_sites_map,
                                                                     RuleBook& rule_book,
                                                                     bool no_update_locations)
{
    if (debug_)  std::cout << "init active sites" << std::endl;
    Eigen::VectorXi update_location_linear = update_location(Eigen::all,1) + rule_book.W_ * update_location(Eigen::all,0);
    if (debug_)  std::cout << "init active sites" << std::endl;

    // time before rulebook
    std::chrono::_V2::system_clock::time_point start, finish, global_start, global_finish ;
    std::chrono::duration<double> time_span;
    
    start = std::chrono::high_resolution_clock::now();
//    global_start = std::chrono::high_resolution_clock::now();
    if (debug_)  std::cout << "init active sites" << std::endl;

    int num_update_locations = update_location.rows();
    Eigen::Matrix<bool,-1,1> bool_new_active_site = Eigen::Matrix<bool,-1,1>::Constant(num_update_locations, false);
    Eigen::Matrix<bool,-1,1> zero_input_update = Eigen::Matrix<bool,-1,1>::Constant(num_update_locations, false);

    if (first_layer_)
    {
        zero_input_update = input_feature_map(update_location_linear, Eigen::all).array().abs().rowwise().sum() == 0; 
        bool_new_active_site = old_input_feature_map_(update_location_linear, Eigen::all).array().abs().rowwise().sum() == 0;

        if (debug_)  std::cout << "Zero input update: " << zero_input_update.transpose()<< std::endl;    
        if (debug_)  std::cout << "bool_new_active_site: " << bool_new_active_site.transpose()<< std::endl;    
    }

//    finish = std::chrono::high_resolution_clock::now();
//    time_span = finish - start;
//    std::cout << "time before rulebook: " << time_span.count() << std::endl;
//    start = std::chrono::high_resolution_clock::now();
 

    Eigen::VectorXi new_update_location_linear;

    if (!no_update_locations)
    {
        updateRulebooks(bool_new_active_site,
                        zero_input_update,
                        update_location_linear,
                        active_sites_map,
                        new_update_location_linear,
                        rule_book);
     }

    Eigen::MatrixXi new_update_location(new_update_location_linear.rows(),2);
    new_update_location(Eigen::all, 0) = new_update_location_linear.unaryExpr([&](const int x) { return x / rule_book.W_; });
    new_update_location(Eigen::all, 1) = new_update_location_linear.unaryExpr([&](const int x) { return x % rule_book.W_; });

    
    if (debug_)  rule_book.print();
 
    finish = std::chrono::high_resolution_clock::now();
    time_span = finish - start;
//    std::cout << "time during rulebook: " << time_span.count() << "     " << float(time_span.count()) << std::endl;
//    start = std::chrono::high_resolution_clock::now();

    Eigen::MatrixXf output_feature_map = output_feature_map_;

    bool first = true;
    for (int kernel_index=0; kernel_index<filter_volume_; kernel_index++)
    {
        int nrules = rule_book.nrules(kernel_index);
        if (nrules == 0)
            continue;

        Eigen::MatrixXf matrix = weights_(kernel_index, Eigen::all).reshaped(nOut_, nIn_);

        std::vector<int> input, output;
        rule_book.getRules(kernel_index, input, output);

        Eigen::MatrixXf delta_feature(nrules, nIn_);
        
        for (int r=0; r<nrules; r++) 
        {
            if (active_sites_map(output[r]) != Site::NEW_ACTIVE)
            {
                delta_feature(r, Eigen::all) = input_feature_map(input[r], Eigen::all) - old_input_feature_map_(input[r], Eigen::all);
            }
            else
            {
                delta_feature(r, Eigen::all) = input_feature_map(input[r], Eigen::all);
            }

            //std::cout << (matrix * delta_feature(r, Eigen::all).transpose()).transpose() << std::endl;
            output_feature_map(output[r], Eigen::all) += matrix * delta_feature(r, Eigen::all).transpose();;
            
        }
        //Eigen::MatrixXf update_term = matrix * delta_feature.transpose();
        //std::cout << "A "<< update_term.transpose() << std::endl;
        //std::cout << "B "<< update_term1.transpose() << std::endl;
        

        if (first)
        {
            first = false;
            if (debug_)  std::cout << "Feature delta: " << delta_feature << std::endl;
            if (debug_)  std::cout << "Weights: " << weights_(kernel_index, Eigen::all).reshaped(nOut_, nIn_) << std::endl;
        }           

        if (debug_)  std::cout << "output_feature_map: " << output_feature_map.sum() << std::endl;
    }
//    finish = std::chrono::high_resolution_clock::now();
//    time_span = finish - start;
//    std::cout << "time during update: " << time_span.count() << std::endl;
//    start = std::chrono::high_resolution_clock::now();

    for (int i=0; i<active_sites_map.rows(); i++)
    {
        if (active_sites_map(i) == Site::NEW_INACTIVE)
        {
            for (int j=0; j<nOut_; j++)
                output_feature_map(i,j) = 0;
        }
        if (use_bias_ && active_sites_map(i) == Site::NEW_ACTIVE)
            output_feature_map(i, Eigen::all) += bias_;
    }

    if (debug_)  std::cout << "output and input feature_map after bias: " << output_feature_map.sum() << " " << input_feature_map.sum() << std::endl;

    old_input_feature_map_ = input_feature_map;
    output_feature_map_ = output_feature_map;
    
//    finish = std::chrono::high_resolution_clock::now();
//    time_span = finish - start;
//    std::cout << "time after rulebook: " << time_span.count() << std::endl;

//    global_finish = std::chrono::high_resolution_clock::now();
//    time_span = global_finish - global_start;
//    std::cout << "global time: " << time_span.count() << std::endl;

    if (debug_)  std::cout << "output and input feature_map after bias: " << new_update_location.cols() << " " << new_update_location.rows() << std::endl;
    if (debug_)  std::cout << new_update_location << std::endl;

//    std::cout << "Time for Rulebook: " << output_feature_map(0, 0) << std::endl;

    return {new_update_location, output_feature_map, active_sites_map};
}

std::tuple<Eigen::MatrixXi, Eigen::MatrixXf, Eigen::MatrixXf, RuleBook> 
AsynSparseConvolution2D::forward(const Eigen::MatrixXi& update_location, 
                                 const Eigen::MatrixXf& feature_map, 
                                 Eigen::Ref<ActiveMatrix> active_sites_map, 
                                 RuleBook& rulebook)
{
    int H = feature_map.rows();
    int W = feature_map.cols() / nIn_;

    bool no_updates = (update_location.rows() == 0);
    
    // If no updates, initialize with zero 
    Eigen::MatrixXi updated_location;
    if (no_updates)
    {
        updated_location = Eigen::MatrixXi::Zero(1, 2);
    }
    else
    {
        updated_location = update_location;
    }

    Eigen::MatrixXf reshaped_feature_map = feature_map;
    reshaped_feature_map.resize(Eigen::NoChange, nIn_);
    
    if (first_layer_)
    {
        initMaps(H, W);
        active_sites_map = initActiveMap(reshaped_feature_map, updated_location);
        rulebook.initialize(H, W, filter_size_, dimension_);
    }
    else
    {
        initMaps(H, W);
        // Flatten active sites map
        active_sites_map.resize(Eigen::NoChange, 1);
    }

    // Here, the actual convolution computation would happen
    // Assuming that the function signature of the C++ version is similar to Python version
    Eigen::MatrixXi new_update_locations;
    Eigen::MatrixXf output_map;
    std::tie(new_update_locations, output_map, active_sites_map) = compute(update_location, reshaped_feature_map, active_sites_map, rulebook, no_updates);

    output_map.resize(H, W, nOut_);
    active_sites_map.resize(H, W);

    return std::make_tuple(new_update_locations, output_map, active_sites_map, rulebook);
}
