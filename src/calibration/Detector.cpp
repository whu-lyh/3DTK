//
// Created by Joschka van der Lucht on 28.02.18.
//


#include"calibration/Detector.h"

namespace calibration {
    void Detector::detectAprilTag(image_u8_t *image, std::vector<AprilTag::AprilTag2f> &tags, float decimate = 1,
                                  float blur = 0.8,
                                  int threads = 4, bool debug = false, bool refine_edges = true,
                                  bool refine_decodes = true,
                                  bool refine_pose = true, std::string tagFamily = "tag36h11") {
        apriltag_family_t *tagFam;
        //set tag family
        if (tagFamily.compare("tag36h11") == 0) {
            tagFam = tag36h11_create();
        } else if (tagFamily.compare("tag36h10") == 0) {
            tagFam = tag36h10_create();
        } else if (tagFamily.compare("tag25h9") == 0) {
            tagFam = tag25h9_create();
        } else if (tagFamily.compare("tag25h7") == 0) {
            tagFam = tag25h7_create();
        } else if (tagFamily.compare("tag16h5") == 0) {
            tagFam = tag16h5_create();
        } else {
            std::cout << "error: false tagFamily, only tag36h11, tag36h10, tag25h9, tag25h7 and tag16h5 supported"
                      << std::endl;
            return;
        }
        //init AprilTag detector
        apriltag_detector_t *apriltagDetector;
        apriltag_detector_add_family(apriltagDetector, tagFam);
        apriltagDetector->quad_decimate = decimate;
        apriltagDetector->quad_sigma = blur;
        apriltagDetector->nthreads = threads;
        apriltagDetector->debug = debug;
        apriltagDetector->refine_edges = refine_edges;
        apriltagDetector->refine_decode = refine_decodes;
        apriltagDetector->refine_pose = refine_pose;
        //stop time for detect tags
        timeval start, end;
        gettimeofday(&start, 0);
        zarray_t *detections = apriltag_detector_detect(apriltagDetector, image);
        gettimeofday(&end, 0);
        std::cout << "Time to detect aprilTags: " << end.tv_sec - start.tv_sec << " sec" << std::endl;

        if (detections == nullptr) {
            std::cout << "detection nullptr, no tag found";
        } else {
            for (int i = 0; i < zarray_size(detections); i++) {
                apriltag_detection_t *det;
                zarray_get(detections, i, &det);
                AprilTag::AprilTag2f aprilTag2f = AprilTag::AprilTag2f(det->id);
                aprilTag2f.point4 = Point2f((float) ((det->p)[0][0]), (float) ((det->p)[0][1]));
                aprilTag2f.point3 = Point2f((float) ((det->p)[1][0]), (float) ((det->p)[1][1]));
                aprilTag2f.point2 = Point2f((float) ((det->p)[2][0]), (float) ((det->p)[2][1]));
                aprilTag2f.point1 = Point2f((float) ((det->p)[3][0]), (float) ((det->p)[3][1]));
                tags.push_back(aprilTag2f);
                apriltag_detection_destroy(det);
            }
            zarray_destroy(detections);
        }
        apriltag_detector_destroy(apriltagDetector);
    }

    void Detector::detectChessboard(Mat &image, std::vector<cv::Point2f> &points, Size boardSize) {
        bool found = findChessboardCorners(image, boardSize, points,
                                           CV_CALIB_CB_ADAPTIVE_THRESH | CV_CALIB_CB_NORMALIZE_IMAGE |
                                           CV_CALIB_CB_FILTER_QUADS);
        timeval start, end;
        if (found) {
            std::cout << "Performing subpixel refinement of chessboard corners." << std::endl;
            cornerSubPix(image, points, cvSize(11, 11), cvSize(-1, -1),
                         cvTermCriteria(CV_TERMCRIT_EPS + CV_TERMCRIT_ITER, 30, 0.1));
        }

        gettimeofday(&end, 0);
        std::cout << "Time to detect chessboard: " << end.tv_sec - start.tv_sec << " sec" << std::endl;
    }
}
