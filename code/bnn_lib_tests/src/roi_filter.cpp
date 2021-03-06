/******************************************************************************
 * Code developed by Elim Kwan in April 2020 
 *
 * Region-Of-Interest Detection (ROI Filter)
 * Detection ROI of Image with Contour Detection/Optical Flow/Hybrid of the two(eff-roi)
 * 
 *****************************************************************************/
#include "roi_filter.hpp"

/*---------------------------------------------------------------------------
-------------------------Contour Detection-----------------------------------
---------------------------------------------------------------------------*/

Rect Roi_filter::basic_roi(const Mat& mat){
/*
	Apply contour detection to the input images. 
    Generate a bounding box on the contour found. 
    Add offset to ROI and snap to edges if neccessary. 

    @param mat: Current Frame
    :return: Rectangle indicating the ROI
*/

    cv::Mat cur = mat.clone();

    cv::Mat grey_mat, grad_x, grad_y, abs_grad_x, abs_grad_y, sobel_mat, canny_mat;
    int scale = 1;
	int delta = 0;
	int ddepth = CV_16S;

    //Convert img to grey scale
    cv::cvtColor(mat, grey_mat, CV_BGR2GRAY);

    //Blur img
    GaussianBlur(grey_mat, grey_mat, Size(3,3), 0, 0, BORDER_DEFAULT );

    //Sobel
    /// Gradient X
	Sobel( grey_mat, grad_x, ddepth, 1, 0, 3, scale, delta, BORDER_DEFAULT );
	/// Gradient Y
	Sobel( grey_mat, grad_y, ddepth, 0, 1, 3, scale, delta, BORDER_DEFAULT );
	convertScaleAbs( grad_x, abs_grad_x );
	convertScaleAbs( grad_y, abs_grad_y );
	addWeighted( abs_grad_x, 0.5, abs_grad_y, 0.5, 0, sobel_mat);

    //Canny
    int thresh = 100;
    Canny( sobel_mat, canny_mat, thresh, thresh*2);

    // imshow("Canny", canny_mat);
    // waitKey(0);

    //Group contour
    double max_area = 0;
    vector<vector<Point> > contours;
    findContours(canny_mat, contours, CV_RETR_EXTERNAL, CHAIN_APPROX_SIMPLE);

    int num_of_contours = contours.size();

    if (contours.size() <= 0){
        cout << "---Too dark to extract contours--" << endl;
        //too dark cant extract any contours
        return get_full_roi();
    }

    vector<vector<Point> > contours_poly(contours.size());
    Rect boundRect;

    for( size_t i = 0; i < contours.size(); i++ )
    {
        approxPolyDP( contours[i], contours_poly[i], 3, true );
    }
    if (contours_poly.size() <= 0){
        //too dark cant extract any contours
        return get_full_roi();
    }

    int k = contours_poly.size();
    
    Rect max_r = boundingRect(contours_poly[k-1]);

    int x1,y1,x2,y2;
    x1 = max_r.x;
    y1 = max_r.y;
    x2 = max_r.x + max_r.width;
    y2 = max_r.y + max_r.height;
    if (k > 1){
        for(size_t i = 0 ; i < k - 2 ; i++){
        if (contours_poly[i].size() > 1){
            double a = contourArea(contours_poly[i]);
            if (a > 10){

                Rect r1 = boundingRect(contours_poly[i]);

                x1 = min(x1,r1.x);
                y1 = min(y1,r1.y);
                x2 = max(x2,r1.x + r1.width);
                y2 = max(y2,r1.y + r1.height);


            }
        }
    }
    }

    small_bounding_r = expand_r(x1,y1,x2,y2, 0.1);

    Rect wrapper = expand_r(x1*4,y1*4,x2*4,y2*4,0.1);

    past_roi = wrapper;
    return wrapper;

}

Rect Roi_filter::expand_r(int x1, int y1, int x2, int y2, float p){
/*
	Add offset to the detected ROI.
    Snap rectangle to edge if they are invalid edges. (Error Prevention)

	@param x1,y1,x2,y2: x, y coordinate of the primary ROI
    @param p: arbitary offset to be added to the primary ROI
    :return: Rectangle indicating the ROI
*/
    
    int a = x1 - x1*p;
    int b = y1 - y1*p;
    int c = x2 + x2*p;
    int d = y2 + y2*p;

    if ( a < 0 || a > frame_width){
        a = 0;
    }
    if ( b < 0 || b > frame_height){
        b = 0;
    }

    if ( c < 0 || c > frame_width || c < a){
        c = frame_width;
    }

    if ( d < 0 || d > frame_height || d < b){
        d = frame_height;
    }

    Rect R(Point(a,b), Point(c, d));

    return R;
}

/*---------------------------------------------------------------------------
-------------------------Optical Flow----------------------------------------
---------------------------------------------------------------------------*/
void Roi_filter::init_enhanced_roi(const Mat& img){
/*
	Optical Flow detection algorithms compares consecutive frames. 
    Hence, this func stores input image as prevous_mat, preparing for possible comparision for the next frame. 

	@param mat: Current Frame
*/
    cv::Mat grey_mat;
    prev_mat = img.clone();
    cvtColor(img,grey_mat, COLOR_BGR2GRAY);
    prev_mat_grey = grey_mat.clone();
}

Rect Roi_filter::enhanced_roi (const Mat& img){
/*
	Main wrapper function for using Optical Flow algorithms to generate ROI.
    Process: Optical Flow Algo to generate motion map -> Edge Detection on motion map -> Colour Similarity Check as Sanity Check

	@param img: Current Frame
*/
    cv::Mat grey;
    cur_mat = img.clone();
    cvtColor(img, grey, COLOR_BGR2GRAY);
    cur_mat_grey = grey.clone();

    cv::Mat contour_mat, motion_mat, weighted_mat;
    Rect bounding_r;

    motion_mat = simple_optical_flow();

    bounding_r = basic_roi(motion_mat);

    int certainty = colour_similarity(small_bounding_r, hsv_motion_mat, 10);//sanity check

    if (certainty == 0){
        bounding_r = past_roi;
    } else{
        prev_mat = cur_mat.clone();
        prev_mat_grey = cur_mat_grey.clone();
    }

    //update past_roi
    past_roi = bounding_r;

    return bounding_r;
}

cv::Mat Roi_filter::simple_optical_flow(){
/*
	Dense Optical Flow Algo with reference to OpenCv documentation (https://docs.opencv.org/3.4/d4/dee/tutorial_optical_flow.html)
    Colour represent the direction of mation, Intensity represent the magnitude of the motion.

	:return: Motion Map
*/

    cv::Mat cur, prev;
    cur = cur_mat_grey.clone();
    prev = prev_mat_grey.clone();

    Mat flow(prev.size(), CV_32FC2);
    calcOpticalFlowFarneback(prev, cur, flow, 0.5, 3, 15, 3, 5, 1.2, 0);

    // visualization
    Mat flow_parts[2];
    split(flow, flow_parts);
    Mat magn, angle, magn_norm;
    cartToPolar(flow_parts[0], flow_parts[1], magn, angle, true);

    normalize(magn, magn_norm, 0.0f, 1.0f, NORM_MINMAX);
    angle *= ((1.f / 360.f) * (180.f / 255.f));

    //build hsv image
    Mat _hsv[3], hsv, hsv8, bgr;
    _hsv[0] = angle; //hue
    _hsv[1] = Mat::ones(angle.size(), CV_32F); //saturation
    _hsv[2] = magn_norm; //value
    merge(_hsv, 3, hsv);
    hsv.convertTo(hsv8, CV_8U, 255.0);

    hsv_motion_mat = hsv8.clone();

    cvtColor(hsv8, bgr, COLOR_HSV2BGR);

    return bgr;
}

int Roi_filter::colour_similarity(Rect r, const Mat& a, int n){
/*
	Motion Map with a single moving object in frame will show a clustor of pixels with similar colour. Otherwise, if there is no motion, it will show random colours.
    By comparing the HSV values of pixels within the detected ROI, we can estimate how likely there are motion, and if the ROI detected contains the moving object.
    As Sanity Check. 

    @param r: ROI detected from Edge Detection on Motion Map
    @param a: Motion Map (assume it is hsv image)
    @param n: Number of pixels to be checked
	:return: integer indicating level of certainty of the motion map is representing a moving object, not just random colour
*/
    srand(11); //set random seed for constant exp. result

    int centre_x, centre_y, ran_x, ran_y;
    unsigned char h,s,v, h2, s2, v2;
    vector<float> h_values, v_values;
    float h_en, v_en;    

    int i = 0;
    int sign = 1;
    while(i < n){

        if (int(r.width/4) == 0 || int(r.height/4) == 0){
            return 0;
        }

        ran_x = int(r.x + int(r.width/2) +  sign*rand()%(int(r.width/4)));
        ran_y = int(r.y + int(r.height/2) + sign*rand()%(int(r.height/4)));

        Vec3b p2 = a.at<Vec3b>(ran_y,ran_x);

        h2 = p2[0];
        v2 = p2[2];

        h_values.push_back(h2);
        v_values.push_back(v2);

        sign = (-1)*(sign);
        i++;
    }

    h_values = normalise(h_values);
    v_values = normalise(v_values);

    h_en = entropy(h_values);
    v_en = entropy(v_values);

    if (h_en <= 0.5 && v_en <= 1.0){
        return 2; //very certain
    } else if (h_en <= 0.5){
        return 1; //certain
    } else {
        return 0; //not certain
    }

}

std::vector<float> Roi_filter::normalise(std::vector<float> &cp){
/*
	Normalisation Func

    @param cp: array
	:return: normalised array
*/
    int mx = *max_element(std::begin(cp), std::end(cp));

    if (mx == 0){
        mx = 1;
    }
    
    for(auto &elem : cp)
        elem = (float)elem / mx;
    return cp;
}

float Roi_filter::entropy(std::vector<float> &arg_vec){
/*
	Entropy Func

    @param cp: array
	:return: an integer representing the entropy of the distribution
*/
    float sum = 0;
    for(auto const &elem : arg_vec){
        sum += elem * std::log2(1/elem);
    }
    return sum;
}

/*---------------------------------------------------------------------------
-------------------------Others Func----------------------------------------
---------------------------------------------------------------------------*/
Rect Roi_filter::naive_roi(const Mat& img, unsigned int roi_size){
/*
	ROI Detection from previous system: return center part of the image as roi

	@param img: Frame
    @param roi_size: ROI size
    :return: Rectangle indicating the ROI
*/
    Rect R(Point((frame_width/2)-(roi_size/2), (frame_height/2)-(roi_size/2)), Point((frame_width/2)+(roi_size/2), (frame_height/2)+(roi_size/2)));
    return R;
}

Rect Roi_filter::get_past_roi(){
/*
	Return ROI result of the previous frame

    :return: Rectangle indicating the ROI
*/
    return past_roi;
}

Rect Roi_filter::get_full_roi(){
/*
	Return the full frame as ROI

    :return: Rectangle indicating the ROI
*/
    Rect R(Point(0,0), Point(frame_width, frame_height));
    past_roi = R;
    return R;
}

void Roi_filter::print_vector(std::vector<Point> &vec)
{
/*
	Helper func for printing vector

	@param &vec: Vector to be printed
*/
	std::cout << "{ ";
	for(auto const &elem : vec)
	{
		std::cout << elem << " ";
	}
	std::cout << "}" <<endl;
}