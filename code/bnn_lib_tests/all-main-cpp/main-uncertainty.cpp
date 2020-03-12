/******************************************************************************
 *  Copyright (c) 2016, Xilinx, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  1.  Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *  2.  Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *  3.  Neither the name of the copyright holder nor the names of its
 *      contributors may be used to endorse or promote products derived from
 *      this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 *  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 *  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 *  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 *  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 *  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 *  OR BUSINESS INTERRUPTION). HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 *  WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 *  OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 *  ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *****************************************************************************/
/******************************************************************************
 *
 *
 * @file main_python.c
 *
 * Host code for BNN, overlay CNV-Pynq, to manage parameter loading, 
 * classification (inference) of single and multiple images
 * 
 *
 *****************************************************************************/
#include "../tiny_cnn/tiny_cnn.h"
#include "../tiny_cnn/util/util.h"
#include <iostream>
#include <fstream>
#include <string.h>
#include <chrono>
#include "foldedmv-offload.h"
#include <algorithm>
#include "opencv2/opencv.hpp"
#include <unistd.h>  		//for sleep
#include <omp.h>  		//for sleep
#include <sys/mman.h> //for clock
#include <sys/types.h> //for clock
#include <sys/stat.h>//for clock
#include <fcntl.h>//for clock
#include <stdio.h>//for clock
#include <stdlib.h>//for clock
//#include <opencv2/core/utility.hpp>

#include <main.hpp>
#include "load.hpp"
#include "roi_filter.hpp"
#include "win.hpp"
#include "uncertainty.hpp"


using namespace std;
using namespace tiny_cnn;
using namespace tiny_cnn::activation;
using namespace cv;
using namespace load;
using namespace basic;

#define frame_width 320		//176	//320	//640
#define frame_height 240		//144	//240	//480

#define HW_ADDR_GPIO 0xF8000170 //base: 0xF8000000 relative: 0x00000170 absolute: 0xF8000170 // ultrasclae+: 0xFF5E00C0
#define MAP_SIZE 4096UL
#define MAP_MASK (MAP_SIZE - 1)

float lambda;
unsigned int ok, failed; // used in FoldedMV.cpp

const std::string USER_DIR = "/home/xilinx/jose_bnn/bnn_lib_tests/";
const std::string BNN_PARAMS = USER_DIR + "params/cifar10/";
//const std::string TEST_DIR = "/home/xilinx/jose_bnn/bnn_lib_tests/experiments/";

ofstream myfile;

//main functions
int classify_frames(unsigned int no_of_frame, string uncertainty_config, bool dropf_config, bool win_config, string roi_config, unsigned int expected_class);


template<typename T>
inline void print_vector(std::vector<T> &vec)
{
	std::cout << "{ ";
	for(auto const &elem : vec)
	{
		std::cout << elem << " ";
	}
	std::cout << "}" <<endl;
}

// double clockToMilliseconds(clock_t ticks){
//     // units/(units/time) => time (seconds) * 1000 = milliseconds
//     return (ticks/(double)CLOCKS_PER_SEC)*1000.0;
// }

// Convert matrix into a vector 
// |1 0 0|
// |0 1 0| -> [1 0 0 0 1 0 0 0 1]
// |0 0 1|
template<typename T>
void flatten_mat(cv::Mat &m, std::vector<T> &v)
{
	if(m.isContinuous()) 
	{
		//cout<< "data is continuous"<< endl;
		v.assign(m.datastart, m.dataend);
	} 
	else 
	{
		cout<< "data is not continuous"<< endl;
		for (int i = 0; i < m.rows; ++i) 
		{
			v.insert(v.end(), m.ptr<T>(i), m.ptr<T>(i)+m.cols);
		}
	}
}

void makeNetwork(network<mse, adagrad> & nn) {
	nn
	#ifdef OFFLOAD
		<< chaninterleave_layer<identity>(3, 32*32, false)
		<< offloaded_layer(3*32*32, 10, &FixedFoldedMVOffload<8, 1>, 0xdeadbeef, 0)
	#endif
		;
}


extern "C" void load_parameters(const char* path)
{
	#include "config.h"
	FoldedMVInit("cnv-pynq");
	network<mse, adagrad> nn;
	makeNetwork(nn);
			cout << "Setting network weights and thresholds in accelerator..." << endl;
			FoldedMVLoadLayerMem(path , 0, L0_PE, L0_WMEM, L0_TMEM);
			FoldedMVLoadLayerMem(path , 1, L1_PE, L1_WMEM, L1_TMEM);
			FoldedMVLoadLayerMem(path , 2, L2_PE, L2_WMEM, L2_TMEM);
			FoldedMVLoadLayerMem(path , 3, L3_PE, L3_WMEM, L3_TMEM);
			FoldedMVLoadLayerMem(path , 4, L4_PE, L4_WMEM, L4_TMEM);
			FoldedMVLoadLayerMem(path , 5, L5_PE, L5_WMEM, L5_TMEM);
			FoldedMVLoadLayerMem(path , 6, L6_PE, L6_WMEM, L6_TMEM);
			FoldedMVLoadLayerMem(path , 7, L7_PE, L7_WMEM, L7_TMEM);
			FoldedMVLoadLayerMem(path , 8, L8_PE, L8_WMEM, L8_TMEM);
}

extern "C" unsigned int inference(const char* path, unsigned int results[64], int number_class, float *usecPerImage)
{

	FoldedMVInit("cnv-pynq");

	network<mse, adagrad> nn;

	makeNetwork(nn);
	std::vector<label_t> test_labels;
	std::vector<vec_t> test_images;

	parse_cifar10(path, &test_images, &test_labels, -1.0, 1.0, 0, 0);
	std::vector<unsigned int> class_result;
	float usecPerImage_int;
	class_result=testPrebuiltCIFAR10_from_image<8, 16>(test_images, number_class, usecPerImage_int);
	if(results)
		std::copy(class_result.begin(),class_result.end(), results);
	if (usecPerImage)
		*usecPerImage = usecPerImage_int;
	return (std::distance(class_result.begin(),std::max_element(class_result.begin(), class_result.end())));
	}

	extern "C" unsigned int inference_test(const char* path, unsigned int results[64], int number_class, float *usecPerImage,unsigned int img_num)
	{

	FoldedMVInit("cnv-pynq");

	network<mse, adagrad> nn;

	makeNetwork(nn);
	std::vector<label_t> test_labels;
	std::vector<vec_t> test_images;

	parse_cifar10(path, &test_images, &test_labels, -1.0, 1.0, 0, 0);
	float usecPerImage_int;

	testPrebuiltCIFAR10<8, 16>(test_images, test_labels, number_class,img_num);


}

extern "C" unsigned int* inference_multiple(const char* path, int number_class, int *image_number, float *usecPerImage, unsigned int enable_detail = 0)
{

	FoldedMVInit("cnv-pynq");

	network<mse, adagrad> nn;

	makeNetwork(nn);

	std::vector<label_t> test_labels;
	std::vector<vec_t> test_images;

	parse_cifar10(path,&test_images, &test_labels, -1.0, 1.0, 0, 0);
	std::vector<unsigned int> all_result;
	std::vector<unsigned int> detailed_results;
	float usecPerImage_int;
	all_result=testPrebuiltCIFAR10_multiple_images<8, 16>(test_images, number_class, detailed_results, usecPerImage_int);
	unsigned int * result;
	if (image_number)
	*image_number = all_result.size();
	if (usecPerImage)
		*usecPerImage = usecPerImage_int;
	if (enable_detail)
	{
		result = new unsigned int [detailed_results.size()];
		std::copy(detailed_results.begin(),detailed_results.end(), result);
	}
	else
	{
		result = new unsigned int [all_result.size()];
		std::copy(all_result.begin(),all_result.end(), result);
	}
	
	return result;
}

extern "C" void free_results(unsigned int * result)
{
	delete[] result;
}

extern "C" void deinit() {
	FoldedMVDeinit();
}


/*
	Command avaliable:
	./BNN 500 en notdrop notflexw fullroi 4 <-for testing various uncertainty scheme
	./BNN 1000 en drop flexw effroi 4

*/
int main(int argc, char** argv)
{
	for(int i = 0; i < argc; i++)
		cout << "argv[" << i << "]" << " = " << argv[i] << endl;	
	
	unsigned int no_of_frame = atoi(argv[1]);
	std::string uncertainty_config = argv[2];
	std::string dropf_config = argv[3];
	std::string win_config = argv[4];
	std::string roi_config = argv[5];
	unsigned int expected_class = (atoi(argv[6]));

	bool dropf_bool = false;
	if (dropf_config == "drop"){
		dropf_bool = true;
	}

	bool win_bool = false;
	if (win_config == "flexw"){
		win_bool = true;
	}


	//configuration of PL clocks
	// cout << "Starting PL clock configuration: " << endl;

	// int memfd;
	// void *mapped_base, *mapped_dev_base;
	// off_t dev_base = HW_ADDR_GPIO; //GPIO hardware


	// memfd = open("/dev/mem", O_RDWR | O_SYNC);
	// if (memfd == -1) {
	// 	printf("Can't open /dev/mem.\n");
	// 	exit(0);
	// }
	// printf("/dev/mem opened for gpio.\n");

	// Map one page of memory into user space such that the device is in that page, but it may not
	// be at the start of the page.
	// mapped_base = mmap(0, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, dev_base & ~MAP_MASK);
	// if (mapped_base == (void *) -1) {
	// 	printf("Can't map the memory to user space.\n");
	// 	exit(0);
	// }
	// printf("GPIO mapped at address %p.\n", mapped_base);


	// get the address of the device in user space which will be an offset from the base
	// that was mapped as memory is mapped at the start of a page

	// mapped_dev_base = mapped_base + (dev_base & MAP_MASK);

	// int* pl_clk = (int*)mapped_dev_base;

	// cout << "Current PL clock configuration: " << hex << *pl_clk << endl;

	//clk_reg_value = strtol(argv[4], NULL, 16); // atoi(argv[4]);
	//*pl_clk = 0x01010B00; //400Mhz
	//*pl_clk = 0x01010C00; //500Mhz for 400m bitstream
	//*pl_clk = 0x01010F00; //400Mhz for 400m bitstream
	//*pl_clk = 0x01020F00; //200Mhz for 400m bitstream
	//*pl_clk = 0x01011600; //200Mhz
	//*pl_clk = 0x00100600; //100Mhz  -- not sure 
	//*pl_clk = clk_reg_value; //100Mhz  -- not sure 
	//*pl_clk = 0x00101800; //200Mhz

	// cout << "New PL clock configuration: " << hex << *pl_clk << endl;
	// cout << dec;

	classify_frames(no_of_frame, uncertainty_config, dropf_bool, win_bool, roi_config, expected_class);

	return 1;
}

int classify_frames(unsigned int no_of_frame, string uncertainty_config, bool dropf_config, bool win_config, string roi_config, unsigned int expected_class){

	myfile.open ("result.csv",std::ios_base::app);
	//myfile << "\nFrame No., Time per frame(us), frame rate (us), Output , Adjusted Output \n";
	myfile << "\nFrame No., camera frame rate (fps), total rate(fps) , period(us), Output , Adjusted Output, cap_time(us), preprocess_time(us), parallel(capnpre)(us), bnn_time(us), window_filter_time(us), uncertainty_time(us), en/var/a , ma, sd, state, mode\n";

    //Initialize variables
	cv::Mat reduced_sized_frame(32, 32, CV_8UC3);
	cv::Mat cur_frame, src, reduced_roi_frame;
	//Mat bnn_input = Mat(frame_size, frame_size, CV_8UC3);
	float_t scale_min = -1.0;
    float_t scale_max = 1.0;
	unsigned int number_class = 10;
	unsigned int output = 0;
	vector<string> classes = {"airplane", "automobile", "bird", "cat", "deer", "dog", "frog", "horse", "ship", "truck"};
    unsigned int size, frame_num = 0;
	tiny_cnn::vec_t outTest(number_class, 0);
	const unsigned int count = 1;
	std::vector<uint8_t> bgr;
	std::vector<std::vector<float> > results_history; //for storing the classification result of previous frame
	float identified = 0.0 , identified_adj = 0.0, total_time = 0.0, total_cap_time = 0.0;

    // Initialize the network 
    deinit();
	load_parameters(BNN_PARAMS.c_str()); 
	printf("Done loading BNN\n");
	FoldedMVInit("cnv-pynq");
	network<mse, adagrad> nn;
	makeNetwork(nn);

    //Allocate memories
    // # of ExtMemWords per input
	const unsigned int psi = 384; //paddedSize(imgs.size()*inWidth, bitsPerExtMemWord) / bitsPerExtMemWord;
	// # of ExtMemWords per output
	const unsigned int pso = 16; //paddedSize(64*outWidth, bitsPerExtMemWord) / bitsPerExtMemWord;
	if(INPUT_BUF_ENTRIES < psi)
	throw "Not enough space in accelBufIn";
	if(OUTPUT_BUF_ENTRIES < pso)
	throw "Not enough space in accelBufOut";
	// allocate host-side buffers for packed input and outputs
	ExtMemWord * packedImages = (ExtMemWord *)sds_alloc((count * psi)*sizeof(ExtMemWord));
	ExtMemWord * packedOut = (ExtMemWord *)sds_alloc((count * pso)*sizeof(ExtMemWord));

    vector<Mat> frames;

	VideoCapture cap(0 + CV_CAP_V4L2);
	if(!cap.open(0))
	{
		cout << "cannot open camera" << endl;
	} 
	cap.set(CV_CAP_PROP_FRAME_WIDTH,frame_width);
	cap.set(CV_CAP_PROP_FRAME_HEIGHT,frame_height);
	//std::cout << "\nCamera resolution = " << cap.get(CV_CAP_PROP_FRAME_WIDTH) << "x" << cap.get(CV_CAP_PROP_FRAME_HEIGHT) << std::endl;
	size = no_of_frame;

	cap >> cur_frame;

	Roi_filter r_filter(frame_width,frame_height);
	r_filter.init_enhanced_roi(cur_frame);

	//Roi_filter optical_f_roi(frame_width,frame_height, cur_frame);

	//output filter with windowing techniques
	Win_filter w_filter(0.2f);
	w_filter.init_weights(0.2f);
	//cout << "size of weight:" << w_filter.wweights.size() << endl;

	Uncertainty u_filter(5);
	Uncertainty var_filter(5);//testing various uncertainty scheme

	int drop_frame_mode = 0;
	int frames_dropped = 0;
	unsigned int adjusted_output = 0;
	cv::Mat display_frame;

    while(frame_num < size){

		auto t0 = chrono::high_resolution_clock::now(); //time statistics

		Rect roi(Point(0,0), Point(frame_width, frame_height));

		//initialize for roi, so when it when to use uncertainty stats for roi, but without dropping frame
		// bool roi_not_dropping_frame = ( drop_frame_mode == 0 || drop_frame_mode == 1 || drop_frame_mode == 2 || (drop_frame_mode == 3 && frames_dropped == 5) || (drop_frame_mode == 4 && frames_dropped == 10));
		// int roi_drop_frame_mode = drop_frame_mode;

		// if (!dropf_config){
		// 	drop_frame_mode = 0; //if not dropping frame, arbitary set drop_frame_mode to zero
		// }

		bool not_dropping_frame = ( drop_frame_mode == 0 || drop_frame_mode == 1 || drop_frame_mode == 2 || (drop_frame_mode == 3 && frames_dropped == 5) || (drop_frame_mode == 4 && frames_dropped == 10));
		
		auto t00 = chrono::high_resolution_clock::now(); //time statistics
		auto temp = chrono::duration_cast<chrono::microseconds>( t00 - t0 ).count();
		auto cap_time = temp;
		auto preprocessing_time = temp;
		auto bnn_time = temp;
		auto uncertainty_time = temp;
		auto wfilter_time = temp;
		auto en_time = temp;
		auto var_time = temp;

		vector<double> u(5, 0.0);
		vector<double> v(5, 0.0);

		#pragma omp parallel sections
		{
			#pragma omp section
			{
				auto t1 = chrono::high_resolution_clock::now(); //time statistics

				cap >> cur_frame;
				display_frame = cur_frame.clone();

				auto t2 = chrono::high_resolution_clock::now();	//time statistics
				cap_time = chrono::duration_cast<chrono::microseconds>( t2 - t1 ).count();
			}

			#pragma omp section
			{
				auto t3 = chrono::high_resolution_clock::now(); //time statistics
				if (roi_config == "effroi"){
					//utilise uncertainty statistic for dynamic roi
					if (frame_num > 1 && (drop_frame_mode == 0 || drop_frame_mode == 1) ){
					//Resizing frame for bnn
					cv::resize(cur_frame, reduced_roi_frame, cv::Size(80, 60), 0, 0, cv::INTER_CUBIC );
					roi = r_filter.basic_roi(reduced_roi_frame, false);
					src = cur_frame(roi);

					cv::resize(src, reduced_sized_frame, cv::Size(32, 32), 0, 0, cv::INTER_CUBIC );
					flatten_mat(reduced_sized_frame, bgr);
					vec_t img;
					std::transform(bgr.begin(), bgr.end(), std::back_inserter(img),[=](unsigned char c) { return scale_min + (scale_max - scale_min) * c / 255; });
					quantiseAndPack<8, 1>(img, &packedImages[0], psi);
				
					} else if (frame_num > 1 && not_dropping_frame){
						roi = r_filter.get_past_roi();
						src = cur_frame(roi);

						//Resizing frame for bnn
						cv::resize(src, reduced_sized_frame, cv::Size(32, 32), 0, 0, cv::INTER_CUBIC );			
						flatten_mat(reduced_sized_frame, bgr);
						vec_t img;
						std::transform(bgr.begin(), bgr.end(), std::back_inserter(img),[=](unsigned char c) { return scale_min + (scale_max - scale_min) * c / 255; });
						quantiseAndPack<8, 1>(img, &packedImages[0], psi);
					}

				} else if (roi_config == "flexroi") {

					//use adaptive roi all the time
					if (frame_num > 1){
						cv::resize(cur_frame, reduced_roi_frame, cv::Size(80, 60), 0, 0, cv::INTER_CUBIC );
						roi = r_filter.basic_roi(reduced_roi_frame, false);
						src = cur_frame(roi);
						cv::resize(src, reduced_sized_frame, cv::Size(32, 32), 0, 0, cv::INTER_CUBIC );
					} else {
						cv::resize(cur_frame, reduced_sized_frame, cv::Size(32, 32), 0, 0, cv::INTER_CUBIC );
					}
					flatten_mat(reduced_sized_frame, bgr);
					vec_t img;
					std::transform(bgr.begin(), bgr.end(), std::back_inserter(img),[=](unsigned char c) { return scale_min + (scale_max - scale_min) * c / 255; });
					quantiseAndPack<8, 1>(img, &packedImages[0], psi);

				} else {

					//use full frame all the time, no roi
					cv::resize(cur_frame, reduced_sized_frame, cv::Size(32, 32), 0, 0, cv::INTER_CUBIC );
					flatten_mat(reduced_sized_frame, bgr);
					vec_t img;
					std::transform(bgr.begin(), bgr.end(), std::back_inserter(img),[=](unsigned char c) { return scale_min + (scale_max - scale_min) * c / 255; });
					quantiseAndPack<8, 1>(img, &packedImages[0], psi);

				}
				//if dropping frame, not going to resize roi and transform it to array
				auto t4 = chrono::high_resolution_clock::now();	//time statistics
				preprocessing_time = chrono::duration_cast<chrono::microseconds>( t4 - t3 ).count();

			}
		}

		auto t5 = chrono::high_resolution_clock::now();	//time statistics
		auto parallel_time = chrono::duration_cast<chrono::microseconds>( t5 - t0 ).count();

		if (!not_dropping_frame && dropf_config){
			frames_dropped +=1;
		} else {
		//reset frames_dropped
			frames_dropped = 0; 

			// Call the hardware function
			kernelbnn((ap_uint<64> *)packedImages, (ap_uint<64> *)packedOut, false, 0, 0, 0, 0, count,psi,pso,1,0);
			if (frame_num != 1)
			{
				kernelbnn((ap_uint<64> *)packedImages, (ap_uint<64> *)packedOut, false, 0, 0, 0, 0, count,psi,pso,0,1);
			}
			// Extract the output of BNN and classify result
			std::vector<float> class_result;
			copyFromLowPrecBuffer<unsigned short>(&packedOut[0], outTest);
			for(unsigned int j = 0; j < number_class; j++) {			
				class_result.push_back(outTest[j]);
			}	
			output = distance(class_result.begin(),max_element(class_result.begin(), class_result.end()));

			auto t6 = chrono::high_resolution_clock::now();	//time statistics
			bnn_time = chrono::duration_cast<chrono::microseconds>( t6 - t5 ).count();

			//Data post-processing:
			//calculate uncertainty
			u = u_filter.cal_uncertainty(class_result,uncertainty_config);
			auto t61 = chrono::high_resolution_clock::now();	//time statistics
			en_time = chrono::duration_cast<chrono::microseconds>( t61 - t6 ).count();

			v = var_filter.cal_uncertainty(class_result,"var");
			auto t62 = chrono::high_resolution_clock::now();	//time statistics
			var_time = chrono::duration_cast<chrono::microseconds>( t62 - t61 ).count();

			drop_frame_mode = u[4];

			auto t7 = chrono::high_resolution_clock::now();	//time statistics
			uncertainty_time = chrono::duration_cast<chrono::microseconds>( t7 - t6 ).count();

			//use window
			w_filter.update_memory(class_result);
			adjusted_output = w_filter.analysis(drop_frame_mode, win_config); //if win_config is true, win_step and length are flexible, else they are fixed to 8 12
			//-------------------------------------------

			auto t8 = chrono::high_resolution_clock::now();	//time statistics
			wfilter_time = chrono::duration_cast<chrono::microseconds>( t8 - t7).count();
		}

		auto t9 = chrono::high_resolution_clock::now();	//time statistics


		std::cout << "-------------------------------------------------"<< endl;
		std::cout << "frame num: " << frame_num << endl;
		std::cout << "adjusted output: " << classes[adjusted_output] << endl;
		std::cout << "-------------------------------------------------"<< endl;

		//cout <<"expected" << expected_class <<" " << adjusted_output <<endl;
		if (int(expected_class) == int(output)){
			identified ++;
		}
		if (int(expected_class) == int(adjusted_output)){
			identified_adj++;
		}

		auto overall_time = chrono::duration_cast<chrono::microseconds>( t9 - t0 ).count();
		float period = (float)overall_time/1000000;
		float cam_fps = 1000000/(float)cap_time;
		float total_fps = 1000000/(float)overall_time;

		string u_stats = to_string(u[0]);
		string u_mode = to_string(u[4]);
		if (u[0] == 0){
			u_stats = "";
			u_mode = "";
		}

		myfile << frame_num << "," << cam_fps << "," << total_fps << "," << period << "," << classes[output] << "," << classes[adjusted_output] << "," << cap_time << "," << preprocessing_time << "," << parallel_time << "," <<  bnn_time << "," << wfilter_time << "," << en_time << "," <<  u_stats << "," << u[1] << "," << u[2] << "," << u[3] << "," << drop_frame_mode << "," << v[0] << "," << v[1] << "," << v[2] << "," << v[3] << "," << v[4] << "," << var_time<< "\n";
		if (frame_num != 0){
			total_time = total_time + period;
			total_cap_time = total_cap_time + (float)cap_time/1000000;
		}

		//Display output
		rectangle(display_frame, roi, Scalar(0, 0, 255));
		putText(display_frame, classes[adjusted_output], Point(15, 55), FONT_HERSHEY_PLAIN, 1, Scalar(0, 255, 0));	
		imshow("Original", display_frame);
		waitKey(25);

		frame_num++;
    }

	float accuracy = 100.0*((float)identified/(float)frame_num);
	float accuracy_adj = 100.0*((float)identified_adj/(float)frame_num);
	float avg_cam_fps = (float)(frame_num-1)/total_cap_time;
	float avg_class_fps = (float)(frame_num-1)/total_time;
	//float avg_rate = 1/((float)win_step*((float)total_time/(float)no_of_frame)); //avg rate for processing win_step number of frame
	cout << identified << " " << frame_num << " " << identified_adj << " " << total_time << " " << no_of_frame << endl;;
	myfile << "\n Accuracy, Adjusted Accuracy, Avg Frame Rate, Avg Classification Rate";
	myfile << "\n" << accuracy << "," << accuracy_adj << "," << avg_cam_fps << "," << avg_class_fps ;
	myfile << "\n \n";
	myfile.close();

	if (cap.open(0)){
		cap.release();
	}

    //Release memory
	//cout<<"debug memory1: " << &packedImages << endl;
    sds_free(packedImages);
	//cout<<"debug memory2: " << &packedOut << endl;
	sds_free(packedOut);
    return 1;
}