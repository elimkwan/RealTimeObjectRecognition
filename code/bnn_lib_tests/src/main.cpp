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

/******************************************************************************

 	Code developed by Elim Kwan in April 2020, modified from Xilinx and Musab Code

	Uses webcam input for classification

	./BNN 24 na notdrop notflexw full-roi ndynclk nbase 1 4 12
	./BNN 24 na notdrop notflexw full-roi ndynclk nbase 1 10 4
	./BNN 24 na notdrop notflexw full-roi ndynclk nbase 1 6 6

	Command avaliable:
	./BNN 500 en notdrop notflexw full-roi ndynclk nbase 4
	./BNN 500 en notdrop notflexw opt-roi ndynclk nbase 4
	./BNN 500 en notdrop notflexw cont-roi ndynclk nbase 4
	./BNN 500 en notdrop notflexw eff-roi ndynclk nbase 4
	./BNN 500 en notdrop flexw eff-roi ndynclk nbase 4
	./BNN 500 en drop notflexw full-roi ndynclk nbase 4
	./BNN 500 en drop notflexw opt-roi ndynclk nbase 4
	./BNN 500 en drop notflexw cont-roi ndynclk nbase 4
	./BNN 500 en drop notflexw eff-roi ndynclk nbase 4
	./BNN 500 en drop flexw eff-roi ndynclk nbase 4
	./BNN 500 en drop flexw eff-roi dynclk nbase 4

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

#include "roi_filter.hpp"
#include "win.hpp"
#include "uncertainty.hpp"


using namespace std;
using namespace tiny_cnn;
using namespace tiny_cnn::activation;
using namespace cv;

#define frame_width 320		//176	//320	//640
#define frame_height 240		//144	//240	//480

#define HW_ADDR_GPIO 0xF8000170 //base: 0xF8000000 relative: 0x00000170 absolute: 0xF8000170 // ultrasclae+: 0xFF5E00C0
#define MAP_SIZE 4096UL
#define MAP_MASK (MAP_SIZE - 1)

float lambda;
unsigned int ok, failed; // used in FoldedMV.cpp

const std::string USER_DIR = "/home/xilinx/jose_bnn/bnn_lib_tests/";
const std::string BNN_PARAMS = USER_DIR + "params/cifar10/";
const std::string TEST_DIR = "/home/xilinx/jose_bnn/bnn_lib_tests/experiments/";

ofstream myfile;
ofstream fs;

//main functions
//int classify_frames(unsigned int no_of_frame, string uncertainty_config, bool dropf_config, bool win_config, string roi_config, bool dynclk, unsigned int expected_class, bool base, int win_step, int win_length);
int classify_frames(unsigned int no_of_frame, string uncertainty_config, int clk_config, int win_step, int win_length, int aa, int bb, int cc, int dd, int ee, int ff, int gg, int hh, int ii, int jj);
void config_clock(int desired_frequency);

/*
--------------------------------------------------------------------------------------------------------------------------
----------------------------------------------------Hardware Functions:---------------------------------------------------
--------------------------------------------------Code from Musab and Xilinx----------------------------------------------
--------------------------------------------------------------------------------------------------------------------------
*/
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
--------------------------------------------------------------------------------------------------------------------------
----------------------------------------------End of Hardware Functions---------------------------------------------------
--------------------------------------------------------------------------------------------------------------------------
*/

/*
--------------------------------------------------------------------------------------------------------------------------
---------------------------Below are student developed code (expect Hardware-related Functions)---------------------------
--------------------------------------------------------------------------------------------------------------------------
*/
int main(int argc, char** argv)
{
/*
	Assign input arguememts to classify function

	@param argc: number of input arguements
	@param argv: vector of input arguements
	:return: an integer
*/
	for(int i = 0; i < argc; i++)
		cout << "argv[" << i << "]" << " = " << argv[i] << endl;

	int aa = atoi(argv[1]);
	int bb = atoi(argv[2]);
	int cc = atoi(argv[3]);
	int dd = atoi(argv[4]);
	int ee = atoi(argv[5]);
	int ff = atoi(argv[6]);
	int gg = atoi(argv[7]);
	int hh = atoi(argv[8]);
	int ii = atoi(argv[9]);
	int jj = atoi(argv[10]);

	classify_frames(1, "na", 1, 1, 1, aa, bb, cc, dd, ee, ff, gg, hh, ii, jj);
	return 1;
}

void config_clock(int fsettings){
/*
	Change Programmable Logic Clock dynamically by changing the register value. This functions is modified from Musab Code.

	@param fsettings: 1 to 5, with 5 being the highest frequency setting
*/
	cout << "Starting PL clock configuration: " << endl;
	int memfd;
	void *mapped_base, *mapped_dev_base;
	off_t dev_base = HW_ADDR_GPIO; //GPIO hardware


	memfd = open("/dev/mem", O_RDWR | O_SYNC);
	if (memfd == -1) {
		printf("Can't open /dev/mem.\n");
		exit(0);
	}
	printf("/dev/mem opened for gpio.\n");

	// Map one page of memory into user space such that the device is in that page, but it may not
	// be at the start of the page.
	mapped_base = mmap(0, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, dev_base & ~MAP_MASK);
	if (mapped_base == (void *) -1) {
		printf("Can't map the memory to user space.\n");
		exit(0);
	}
	printf("GPIO mapped at address %p.\n", mapped_base);

	// get the address of the device in user space which will be an offset from the base
	// that was mapped as memory is mapped at the start of a page
	mapped_dev_base = mapped_base + (dev_base & MAP_MASK);

	int* pl_clk = (int*)mapped_dev_base;

	cout << "Current PL clock configuration: " << hex << *pl_clk << endl;

	int freq = fsettings;
	switch(freq) {
		case 20: *pl_clk = 0x00A00500; break; //20MHz
		case 25: *pl_clk = 0x00A00400; break; //25
		case 33: *pl_clk = 0x00A00300; break; //33MHz
		case 50: *pl_clk = 0x00A00200; break; //50
		case 100: *pl_clk = 0x00A00100; break; //100MHz
		case 111: *pl_clk = 0x00100900; break; //111MHz
		case 125: *pl_clk = 0x00100800; break; //125MHz
		case 143: *pl_clk = 0x00100700; break; //143
    	case 166: *pl_clk = 0x00100600; break; //166MHz
		//default: *pl_clk = 0x00A00500; //20MHz
	}

	// if (fsettings == 5){
	// 	*pl_clk = 0x00100600; //166MHz
	// } else if (fsettings == 4){
	// 	*pl_clk = 0x00100800; //125MHz
	// } else if (fsettings == 3){
	// 	*pl_clk = 0x00A00100; //100MHz
	// } else if (fsettings == 2){
	// 	*pl_clk = 0x00A00300; //33MHz
	// } else if (fsettings == 1){
	// 	*pl_clk = 0x00A00500; //20MHz
	// } else{
	// 	*pl_clk = 0x00A00100;//100MHz
	// }

	cout << "New PL clock configuration: " << hex << *pl_clk << endl;
	cout << dec;

}

int classify_frames(unsigned int no_of_frame, string uncertainty_config, int clk_config, int win_step, int win_length, int aa, int bb, int cc, int dd, int ee, int ff, int gg, int hh, int ii, int jj){
/*
	Main analysis function for classifying the object in frame.

	@param no_of_frame: Number of frames to be processed [10 ... 2000]
	@param uncertainty_config: Choose the uncertainty calculation schemes to be used - [en var a na] 
	@param dropf_config: Choose whether to decimate frames - [True False]
	@param win_config: Choose whether to use flexible window filter - [True False]
	@param roi_config: Choose the ROI schemes to be used - [full-roi cont-roi opt-roi eff-roi]
	@param dynclk: Choose whether to use dynamica PL clock - [True False]
	@param expected_class: Choose the expected in-frame object for accuracy calculation [0 ... 9]
	@param base: Choose whether to adopt base case setting - [True False]
	:return: an integer

*/

	//Set file name for accuracy and timing stats collected
	// string d = "-ndrop";
	// string w = "-nflexw-";
	// string c = "-ndynclk";
	// string ba = "-nbase";
	// if (dropf_config){
	// 	d = "-drop";
	// }
	// if (win_config){
	// 	w = "-flexw-";
	// }
	// if (dynclk){
	// 	c = "-dynclk";
	// }
	// if (base){
	// 	ba = "-base";
	// }


	//Open webcam
	// VideoCapture cap(0 + CV_CAP_V4L2);
	// if(!cap.open(0 + CV_CAP_V4L2))
	// {
	// 	cout << "cannot open camera" << endl;
	// 	return 0;
	// }
	// cap.set(CV_CAP_PROP_FRAME_WIDTH,frame_width);
	// cap.set(CV_CAP_PROP_FRAME_HEIGHT,frame_height);
	// cap >> cur_frame; //will be dropped, just for initialisation

	//[Hardware-Related Functions]Initialize the BNN
	deinit();
	load_parameters(BNN_PARAMS.c_str()); 
	printf("Done loading BNN\n");
	FoldedMVInit("cnv-pynq");
	network<mse, adagrad> nn;
	makeNetwork(nn);

	//Allocate memories
	const unsigned int count = 1;
	float_t scale_min = -1.0;
	float_t scale_max = 1.0;
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

	//vector< vector<int> > win_list = {{5,1}}; //base
	//vector<int> clk_list = {20};

	vector<int> dataset_list = {1,2,3,4,5};
	vector <vector<int> > win_list = {{1,1}};
	vector<string> un_list = {"en"};

	// vector<vector<int>> win_list(225, vector<int> (2, 0)); //15*15 = 100
	// int c = 1;
	// int d = 1;
	// for (int k = 0; k < 100; k++){
	// 	win_list[k][0] = c;
	// 	win_list[k][1] = d;
	// 	if ((k+1)%15 == 0){
	// 		d++;
	// 	}
	// 	if (c != 15){
	// 		c++;
	// 	} else{
	// 		c=1;
	// 	}
	// } ./BNN 1 1 1 1 1 1 1 1 1 1 

	config_clock(20);
	std::string roi_config = "full-roi";
	bool dynclk = false;

	float expected_acc = 66;
	float resultant_acc = 0;
	int exp_count = 0;

	//while(resultant_acc < expected_acc && exp_count< 10){
		fs.open ("./experiments/result/result-overview.csv",std::ios_base::app);
		fs <<  "\n Dataset, Accuracy, Avg Frame Rate, Avg Processing Rate, Avg Classification Rate, Avg BNN latency, Avg BNN latency per classification, Avg Win Time, Avg Win Time per classification, Avg Un Time, Avg Un Time per classification, PL Clk Setting(MHz)";
	

		resultant_acc = 0;
		// aa = 2;
		// bb = 9;
		// cc = (rand() % 11) + 1;
		// dd = (rand() % 11) + 1;
		// ee = 5;
		// ff = 9;
		// gg = 7;
		// hh = 2;
		// ii = 5;
		// jj = 2;

		for (int i = 0; i < dataset_list.size(); i++){

			int folder_num = dataset_list[i];

			//loading dataset
			vector<cv::String> fn;
			string src_dir =  TEST_DIR + "U" + to_string(folder_num) + "/*.png";
			glob(src_dir, fn, false);
			size_t count_fn = fn.size();

			cout << "Dataset" << folder_num << endl;

			for (int j = 0; j < un_list.size(); j ++){
				uncertainty_config = un_list[j];

				for (int k = 0; k < win_list.size(); k++){
					bool win_config = false; //flex
					win_step = win_list[k][0];
					win_length = win_list[k][1];

					//cout << "Window: " << win_step << "-" << win_length << endl;

					// ----------------------------------------------------------------------------------------------------------------
					// ----------------------------------------------------------------------------------------------------------------
					// ----------------------------------------------------------------------------------------------------------------
					
					std::string result_dir = "./experiments/result/dataset" + std::to_string(folder_num) + "-" + uncertainty_config + ".csv";
					myfile.open (result_dir,std::ios_base::app);
					//myfile << "\n" << result_dir;
					myfile << "\nFrame No." << "," << "MA" << "," << "SD" << "\n";
					//myfile << "\nFrame No., inst camera frame rate (fps), Processing Latency(us), Classification rate(fps), Classification Latency(us), Displayed Output, Processed Output, cap(us), roi(us),preprocessing(us), bnn_time(us), un_time(us), win_time(us) \n";

					//Initialize variables
					cv::Mat reduced_sized_frame(32, 32, CV_8UC3);
					cv::Mat cur_frame, src, reduced_roi_frame;
					unsigned int number_class = 10;
					unsigned int output = 0;
					vector<string> classes = {"airplane", "automobile", "bird", "cat", "deer", "dog", "frog", "horse", "ship", "truck"};
					unsigned int frame_num = 0;
					tiny_cnn::vec_t outTest(number_class, 0);
					std::vector<uint8_t> bgr;
					std::vector<std::vector<float> > results_history; //for storing the classification result of previous frame
					float identified = 0.0 , identified_adj = 0.0, total_time = 0.0, total_cap_time = 0.0, total_bnn = 0.0, total_win = 0.0, total_un = 0.0;

					cur_frame = imread(fn[0]);
					//Initialise Roi, Window and Uncertainty Filter
					Roi_filter r_filter(frame_width,frame_height);
					r_filter.init_enhanced_roi(cur_frame);
					Win_filter w_filter(win_step, win_length);
					w_filter.init_weights(0.2f);
					Uncertainty u_filter(5);

					//Initialise variables after webcam and filter initialisation
					int ps_mode = 0;
					int frames_dropped = 0;
					unsigned int adjusted_output = 0;
					cv::Mat display_frame = cur_frame.clone();
					int pastclk = 100;
					float acc_time = 0;
					int processed_frames = 0;
					int cls_frames = 0;
					string display_output = "";
					Rect display_roi(Point(0,0), Point(frame_width, frame_height));
					bool process_frame = true;
					std::vector<float> class_result;
					std::string correct = "";

					//while(frame_num < no_of_frame){
					for (size_t d=0; d<count_fn; d++){
					//for (size_t d=0; d<100; d++){

						Rect roi(Point(0,0), Point(frame_width, frame_height));
						process_frame = !(w_filter.dropf()); //check whether the current frame will be processed
						//std::cout << "-------------------------------------------------"<< endl;
						//std::cout << "Frame Number: " << frame_num << endl;
						float cap_time = 0;
						float preprocessing_time = 0;
						float bnn_time = 0;
						float uncertainty_time = 0;
						float wfilter_time = 0;
						float en_time = 0;
						float var_time = 0;
						vector<double> u(5, 0.0);

						cur_frame = imread(fn[d]);

						auto t0 = chrono::high_resolution_clock::now(); //time statistics
						//auto t1 = chrono::high_resolution_clock::now(); //time statistics

						//Pipeline Capture Frame and ROI code Block with OpenMP Lib
						#pragma omp parallel sections
						{
							#pragma omp section
							{
								//cap >> cur_frame;
								waitKey(6);
								display_frame = cur_frame.clone();

								auto t2 = chrono::high_resolution_clock::now();	//time statistics
								cap_time = chrono::duration_cast<chrono::microseconds>( t2 - t0 ).count();
							}

							#pragma omp section
							{	
								//ROI Functions
								auto t3 = chrono::high_resolution_clock::now(); //time statistics
								if (process_frame){

									if (roi_config == "eff-roi"){

										cv::resize(cur_frame, reduced_roi_frame, cv::Size(80, 60), 0, 0, cv::INTER_CUBIC);
										if (ps_mode != 1){
											r_filter.init_enhanced_roi(reduced_roi_frame);
										}

										if (ps_mode == 0){

											roi = r_filter.get_full_roi();

										}else if (ps_mode == 1){

											roi = r_filter.enhanced_roi(reduced_roi_frame);

										}else if (ps_mode == 2){

											roi = r_filter.get_past_roi();

										}else if (ps_mode == 3){

											roi = r_filter.basic_roi(reduced_roi_frame);

										}else{
											roi = r_filter.get_past_roi();
										}

										src = cur_frame(roi);
										cv::resize(src, reduced_sized_frame, cv::Size(32, 32), 0, 0, cv::INTER_CUBIC );
										flatten_mat(reduced_sized_frame, bgr);
										vec_t img;
										std::transform(bgr.begin(), bgr.end(), std::back_inserter(img),[=](unsigned char c) { return scale_min + (scale_max - scale_min) * c / 255; });
										quantiseAndPack<8, 1>(img, &packedImages[0], psi);

									} else if (roi_config == "opt-roi"){

										cv::resize(cur_frame, reduced_roi_frame, cv::Size(80, 60), 0, 0, cv::INTER_CUBIC );
										//cv::resize(cur_frame, reduced_roi_frame, cv::Size(320, 240), 0, 0, cv::INTER_CUBIC );

										if (frame_num < 2){
											roi = r_filter.get_full_roi();
											r_filter.init_enhanced_roi(reduced_roi_frame);
										} else {
											roi = r_filter.enhanced_roi(reduced_roi_frame);
										}

										src = cur_frame(roi);
										cv::resize(src, reduced_sized_frame, cv::Size(32, 32), 0, 0, cv::INTER_CUBIC );
										flatten_mat(reduced_sized_frame, bgr);
										vec_t img;
										std::transform(bgr.begin(), bgr.end(), std::back_inserter(img),[=](unsigned char c) { return scale_min + (scale_max - scale_min) * c / 255; });
										quantiseAndPack<8, 1>(img, &packedImages[0], psi);


									} else if (roi_config == "cont-roi") {
										
										cv::resize(cur_frame, reduced_roi_frame, cv::Size(80, 60), 0, 0, cv::INTER_CUBIC );
										//cv::resize(cur_frame, reduced_roi_frame, cv::Size(320, 240), 0, 0, cv::INTER_CUBIC );

										if (frame_num < 2){
											roi = r_filter.get_full_roi();
										} else {
											roi = r_filter.basic_roi(reduced_roi_frame);
										}

										src = cur_frame(roi);
										cv::resize(src, reduced_sized_frame, cv::Size(32, 32), 0, 0, cv::INTER_CUBIC );
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
								}
								//if dropping frame, not going to resize roi and transform it to array
								auto t4 = chrono::high_resolution_clock::now();	//time statistics
								preprocessing_time = chrono::duration_cast<chrono::microseconds>( t4 - t3 ).count();

							}
						}
						auto t5 = chrono::high_resolution_clock::now();	//time statistics
						auto parallel_time = chrono::duration_cast<chrono::microseconds>( t5 - t0 ).count();

						//If using dynamic clock, change clock frquency to high settings when level of certainty is low
						// if (dynclk){
						// 	if ( (ps_mode == 4 || ps_mode == 5) && pastclk == 100){
						// 		//high level of certainty
						// 		config_clock(5);
						// 		pastclk = 50;
						// 	} else if ((ps_mode == 0 || ps_mode == 1 || ps_mode == 2 || ps_mode == 3) && pastclk == 50){
						// 		//low level of certainty
						// 		config_clock(1);
						// 		pastclk = 100;
						// 	}
						// }

						if (process_frame){
							//[Hardware-Related Functions] Call the bnn
							auto t66 = chrono::high_resolution_clock::now();	//time statistics
							kernelbnn((ap_uint<64> *)packedImages, (ap_uint<64> *)packedOut, false, 0, 0, 0, 0, count,psi,pso,1,0);
							if (frame_num != 1)
							{
								kernelbnn((ap_uint<64> *)packedImages, (ap_uint<64> *)packedOut, false, 0, 0, 0, 0, count,psi,pso,0,1);
							}
							//Extract the output of BNN and classify result
							//std::vector<float> class_result;
							class_result.clear();
							copyFromLowPrecBuffer<unsigned short>(&packedOut[0], outTest);
							for(unsigned int j = 0; j < number_class; j++) {			
								class_result.push_back(outTest[j]);
							}
							output = distance(class_result.begin(),max_element(class_result.begin(), class_result.end()));

							auto t6 = chrono::high_resolution_clock::now();	//time statistics
							bnn_time = chrono::duration_cast<chrono::microseconds>( t6 - t66 ).count();

							//Data post-processing:
							//calculate uncertainty
							auto t77 = chrono::high_resolution_clock::now();	//time statistics
							u = u_filter.cal_uncertainty(class_result,uncertainty_config, output);
							ps_mode = u[4];
							auto t7 = chrono::high_resolution_clock::now();	//time statistics
							uncertainty_time = chrono::duration_cast<chrono::microseconds>( t7 - t77 ).count();
						} else {
							class_result.clear();
							for(unsigned int j = 0; j < number_class; j++) {			
								class_result.push_back(0);
							}
						}

						auto t8 = chrono::high_resolution_clock::now();	//time statistics
						bool processf = w_filter.processf();
						// if (processf){
						// 	processed_frames += 1;
						// }

						if (process_frame){
							processed_frames += 1;
						}
						//Window Filter
						adjusted_output = w_filter.analysis(class_result,ps_mode, win_config, aa, bb, cc, dd, ee, ff, gg, hh, ii, jj); //if win_config is true, win_step and length are flexible, else they are fixed to 8 12
						auto t9 = chrono::high_resolution_clock::now();	//time statistics
						wfilter_time = chrono::duration_cast<chrono::microseconds>( t9 - t8).count();
						float overall_time = chrono::duration_cast<chrono::microseconds>( t9 - t0 ).count();

						//std::cout << "adjusted output: " << adjusted_output << endl;
						if (adjusted_output > 9){
							adjusted_output = 9;
						}

						//---------------------------------------Below output result to users and stored them on CSV files---------------------------------------------------------------
						// std::cout << "-------------------------------------------------"<< endl;
						// std::cout << "frame num: " << frame_num << endl;
						// std::cout << "adjusted output: " << classes[adjusted_output] << endl;
						// std::cout << "-------------------------------------------------"<< endl;
						
						std::string expected_class = fn[frame_num];
						int first_idx = expected_class.find_last_of('_') + 1;
						expected_class = expected_class.substr(first_idx, expected_class.length()-4);
						expected_class.erase(expected_class.length()-4);

						if (frame_num == 0){
							//myfile << frame_num << "\n" ;
							//imshow("Original", display_frame);
							//waitKey(25);
							frame_num++;
							continue; // exclude first frame from calculation skip the remaining code in the loop
						}

						float cam_fps = 1000000/(float)cap_time;
						std::string r_out, a_out;
						float cls_fps, cls;

						correct = "";
						if (frame_num == 0){
							r_out = " ";
							a_out = " ";
							acc_time = 0;
							cls_fps = 0;
							cls = 0;
						} else if (w_filter.get_display_f()){
							cls_frames ++;

							r_out = classes[adjusted_output];
							display_output = classes[adjusted_output];//update displayed output
							display_roi = roi;
							acc_time += overall_time;

							if (acc_time == 0){ //first frame
								acc_time = overall_time;
								cls_fps = 0;
							} else {
								cls_fps = 1000000/(float)acc_time;
								cls = acc_time; //in us
							}

							if (expected_class == classes[adjusted_output]){
								identified_adj++;
								correct = "correct";
							} else{
								correct = "wrong";
							}
							// else {
							// 	cout << "Wrong when at " << ps_mode << endl;
							// }
							acc_time = 0; //reset accumulated time

						} else {
							r_out = " ";
							acc_time += overall_time;
							cls_fps = 0;
							cls = 0;
						}

						if (frame_num != 0 && processf){
							a_out = classes[adjusted_output];
						} else{
							a_out = " ";
						}

						//myfile << frame_num << "," << cam_fps << "," << overall_time << "," << cls_fps << "," << cls << "," << r_out << "," << a_out << "," << cap_time << "," << preprocessing_time << "," << parallel_time << "," <<  bnn_time << "," << uncertainty_time << "," << wfilter_time << "\n";
						myfile << frame_num << "," << u[1] << "," << u[2] << "\n";
						//myfile << frame_num << "," << a_out << "," << r_out << "," << correct << "," << u[1] << "," << ps_mode << "\n";

						if (frame_num != 0){
							total_time = total_time + (float)overall_time/1000000;
							total_cap_time = total_cap_time + (float)cap_time/1000000;
							total_bnn += (float)bnn_time;
							total_win += (float)wfilter_time;
							total_un += (float)uncertainty_time;
						}

						//Display output
						// rectangle(display_frame, display_roi, Scalar(0, 0, 255));
						// putText(display_frame, display_output, Point(15, 55), FONT_HERSHEY_PLAIN, 1, Scalar(0, 255, 0));	
						// imshow("Original", display_frame);
						// waitKey(25);

						frame_num++;

						char ESC = waitKey(1);	
						if (ESC == 27) 
						{
							cout << "ESC key is pressed by user" << endl;
							break;
						}	
					}

					//exclude first frame from calculation
					float f = (float)frame_num - 1;
					// float pf = (win_length == 1)? ((float)cls_frames-1) : (float)cls_frames;
					// float ppf = (win_length == 1)? ((float)processed_frames-1) : (float)processed_frames;
					float pf = cls_frames-1;
					float ppf = processed_frames-1;

					float accuracy_adj = 100.0*((float)identified_adj/cls_frames);
					float avg_cam_fps = f/total_time;
					float avg_pro_fps = ppf/total_time;
					float avg_cls_fps = pf/total_time;
					float avg_bnn = total_bnn/f;
					float avg_bnn_perc = total_bnn/pf;
					float avg_win = total_win/f;
					float avg_win_perc = total_win/pf;
					float avg_un= total_un/f;
					float avg_un_perc = total_un/pf;

					float temp = avg_cam_fps/win_step;

					//myfile << "\n Step Size, Length, Accuracy, Avg Frame Rate, Avg Processing Rate, Avg Classification Rate, Avg BNN latency, Avg BNN latency per classification, Avg Win Time, Avg Win Time per classification, Avg Un Time, Avg Un Time per classification, PL Clk Setting(MHz)";
					//myfile << "\n" << win_step << "," << win_length << "," << accuracy_adj << "," << avg_cam_fps << "," << avg_pro_fps << "," << avg_cls_fps << "," << avg_bnn << "," << avg_bnn_perc << "," << avg_win << "," << avg_win_perc << "," << avg_un << "," << avg_un_perc << "," << clk_frq << "\n";
					//myfile << cls_frames << "," << processed_frames << "," << avg_pro_fps << "\n";
					//myfile << "\n \n";
					myfile.close();

					// ----------------------------------------------------------------------------------------------------------------
					// ----------------------------------------------------------------------------------------------------------------
					// ----------------------------------------------------------------------------------------------------------------
					
					//fs << "\n" << folder_num << "," << accuracy_adj << "," << avg_cam_fps << "," << avg_pro_fps << "," << avg_cls_fps << "," << avg_bnn << "," << avg_bnn_perc << "," << avg_win << "," << avg_win_perc << "," << avg_un << "," << avg_un_perc << "," << uncertainty_config << "," << aa << "," << bb << "," << cc << "," << dd << "," << ee << "," << ff << "," << gg << "," << hh << "," << ii << "," << jj ;
					fs << "\n" << folder_num << "," << accuracy_adj << "," << avg_cam_fps << "," << avg_pro_fps << "," << avg_cls_fps << "," << avg_bnn << "," << avg_bnn_perc << "," << avg_win << "," << avg_win_perc << "," << avg_un << "," << avg_un_perc << "," << uncertainty_config << ","<< identified_adj << "," << cls_frames;
					
					resultant_acc += accuracy_adj;
				}
			}
		}
		exp_count++;
		resultant_acc = resultant_acc/6;
		cout << "Accuracy " << resultant_acc << endl;
		fs.close();
	//}

	//cap.release();
	config_clock(20); //reset clock to 100MHz
    //[Hardware-Related Functions] Release memory
    sds_free(packedImages);
	sds_free(packedOut);
    return 1;
}