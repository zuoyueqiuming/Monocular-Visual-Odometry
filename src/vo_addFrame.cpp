
#include "my_slam/vo.h"

namespace my_slam
{

void VisualOdometry::addFrame(Frame::Ptr frame)
{
    // Settings
    const int VO_INIT_LENGHTH_UNIT = 100; // num units per meter. (10 for dm, 100 for cm, 1000 for mmm)
    const int FRAME_FOR_FIRST_ESSENTIAL = 14;

    // Renamed vars
    curr_ = frame;
    const int img_id = curr_->id_;
    const Mat &K = curr_->camera_->K_;

    // Vars

    // Start
    while(1){
        printf("\n\n=============================================\n");
        printf("Start processing the %dth image.\n", img_id);

        // Init a curr_. Extract keypoints and descriptors.
        curr_->extractKeyPoints();
        curr_->computeDescriptors();
        cout << "Number of keypoints: " << curr_->keypoints_.size() << endl;

        // vo_state_: BLANK -> INITIALIZATION
        // static Mat 
        if (vo_state_ == BLANK)
        {
            curr_->T_w_c_ = Mat::eye(4, 4, CV_64F);
            curr_->R_curr_to_prev_ = Mat::eye(3, 3, CV_64F);
            curr_->t_curr_to_prev_ = Mat::zeros(3, 1, CV_64F);
            vo_state_ = INITIALIZATION;

            // Update initialization parameters
            init_frame_=curr_;
            init_frame_keypoints_=curr_->keypoints_;
            init_frame_descriptors_=curr_->descriptors_.clone();

        }else if (vo_state_ == INITIALIZATION)
        {
            // Match features
            my_geometry::matchFeatures(init_frame_descriptors_, curr_->descriptors_, curr_->matches_);

            // -- Estimation motion by Essential && Homography matrix and get inlier points
            vector<Mat> list_R, list_t, list_normal;
            vector<vector<DMatch>> list_matches; // these are the inliers matches
            vector<vector<Point3f>> sols_pts3d_in_cam1_by_triang;
            const bool print_res = false, is_frame_cam2_to_cam1 = true;
            const bool compute_homography = true;
            helperEstimatePossibleRelativePosesByEpipolarGeometry(
                /*Input*/
                init_frame_keypoints_, curr_->keypoints_, curr_->matches_, K,
                /*Output*/
                list_R, list_t, list_matches, list_normal, sols_pts3d_in_cam1_by_triang,
                /*settings*/
                print_res, compute_homography, is_frame_cam2_to_cam1);

            // -- Compute errors of results of E/H estimation:
            // [epipolar error] and [trigulation error on norm plane]
            // for the 3 solutions of (E, H1, H2)/
            // Choosing a good solution might based on these criterias.
            vector<double> list_error_epipolar;
            vector<double> list_error_triangulation;
            helperEvaluateEstimationsError(
                init_frame_keypoints_, curr_->keypoints_, list_matches,
                sols_pts3d_in_cam1_by_triang, list_R, list_t, list_normal, K,
                /*output*/
                list_error_epipolar, list_error_triangulation,
                true); // print result

            // Check initialization condition
            // Method 1: manually set an image id
            const bool VO_INIT_FLAG_MANUAL = img_id >= FRAME_FOR_FIRST_ESSENTIAL;

            // Method 2: check the distance between inlier points, run vo until there is a large movement.
            vector<double> dists_between_kpts;
            for (const DMatch &d : list_matches[0])
            {
                Point2f p1 = init_frame_keypoints_[d.queryIdx].pt;
                Point2f p2 = curr_->keypoints_[d.trainIdx].pt;
                dists_between_kpts.push_back(calcDist(p1, p2));
            }
            double mean_dist = 0;
            for (double d : dists_between_kpts)
                mean_dist += d;
            mean_dist /= dists_between_kpts.size();
            const bool VO_INIT_FLAG_KPT_DIST = mean_dist > 50;

            if (VO_INIT_FLAG_KPT_DIST)
            {
                cout << "Large movement detected at curr_ " << img_id << ". Start initialization" << endl;
            }
            else
            { // skip this curr_
                curr_->T_w_c_ = init_frame_->T_w_c_;
                curr_->R_curr_to_prev_ = init_frame_->R_curr_to_prev_;
                curr_->t_curr_to_prev_ = init_frame_->t_curr_to_prev_;
                break;
            }

            // -- Choose 1 solution from the 3 solutions.
            //      Results: R, t, inlier_matches, pts_3d in cam1 and cam2
            // Currently, I'll just simply choose the result from essential matrix.
            //      Need to read papers such as ORB-SLAM2.
            // === analyzeError(list_error_epipolar, list_error_triangulation) ===
            const int SOL_IDX = 0; // 0 corresponds to Essential matrix
            vector<DMatch> &inlier_matches = list_matches[SOL_IDX];
            Mat &R = list_R[SOL_IDX], &t = list_t[SOL_IDX];
            vector<Point3f> &pts3d_in_cam1 = sols_pts3d_in_cam1_by_triang[SOL_IDX];
            vector<Point3f> pts3d_in_cam2;
            for (const Point3f &p1 : pts3d_in_cam1)
                pts3d_in_cam2.push_back(transCoord(p1, R, t));
            const int num_inlier_pts = pts3d_in_cam2.size();

            // -- Normalize the mean depth of points to be 1m
            double mean_depth = 0;
            for (const Point3f &p : pts3d_in_cam2)
                mean_depth += p.z;
            mean_depth /= num_inlier_pts;
            mean_depth = mean_depth / VO_INIT_LENGHTH_UNIT;
            t /= mean_depth;
            for (Point3f &p : pts3d_in_cam2)
            {
                p.x /= mean_depth;
                p.y /= mean_depth;
                p.z /= mean_depth;
            }

            // -- Update current camera pos
            Mat T_curr_to_prev = transRt2T(R, t);
            curr_->T_w_c_ = init_frame_->T_w_c_ * T_curr_to_prev.inv();
            curr_->R_curr_to_prev_ = R;
            curr_->t_curr_to_prev_ = t;
            curr_->inlier_matches_ = inlier_matches;
            curr_->inliers_pts3d_ = pts3d_in_cam2;

            // --Update vo state
            vo_state_ = OK;

            ref_=curr_;
        }
        else if (vo_state_ == OK)
        {

 // Push previous curr_'s keypoints to a local map
            // Match features (Simulate the matching process)

            // Insert inlier_matches' keypoints into the local map
            // map_->map_points_.clear();
            // const vector<DMatch> &prev_inlier_matches = ref_->inlier_matches_;
            // for (int i = 0; i < prev_inlier_matches.size(); i++)
            // { // Match features (Simulate the matching process)

            // Insert inlier_matches' keypoints into the local map
            // map_->map_points_.clear();
            // const vector<DMatch> &prev_inlier_matches = ref_->inlier_matches_;
            // for (int i = 0; i < prev_inlier_matches.size(); i++)
            // {
            //     int pt_idx = prev_inlier_matches[i].trainIdx;
            //     MapPoint::Ptr map_point(new MapPoint(
            //         Point3f_to_Mat(ref_->inliers_pts3d_[i]),
            //         ref_->descriptors_.row(pt_idx).clone()));
            //     map_->insertMapPoint(map_point);
            // }

            // From the local map, find the keypoints that fall into the current view
            // vector<MapPoint::Ptr> candidate_mappoints_in_map;
            // Mat candidate_descriptors_in_map;
            // getMappointsInCurrentView(candidate_mappoints_in_map, candidate_descriptors_in_map);

            // TESTING: Use these matched keypoints to make up a fake ref_
            vector<KeyPoint> tmp_kpts;
            Mat tmp_descriptors;
            vector<DMatch> tmp_matches;
            vector<DMatch> tmp_inlier_matches;
            for (int i = 0; i < ref_->inlier_matches_.size(); i++)
            {
                int pt_idx = ref_->inlier_matches_[i].trainIdx;
                tmp_kpts.push_back(ref_->keypoints_[pt_idx]);
                tmp_descriptors.push_back(ref_->descriptors_.row(pt_idx).clone());

                DMatch tmp_dmatch = ref_->matches_[i];
                tmp_dmatch.trainIdx = i;
                tmp_matches.push_back(tmp_dmatch);

                DMatch tmp_inlier_dmatch = ref_->inlier_matches_[i];
                tmp_inlier_dmatch.trainIdx = i;
                tmp_inlier_matches.push_back(tmp_inlier_dmatch);
            }
            ref_->keypoints_ = tmp_kpts;
            ref_->descriptors_ = tmp_descriptors;
            ref_->matches_ = tmp_matches;
            ref_->inlier_matches_ = tmp_inlier_matches;

        
            // Match features
            curr_->matchFeatures(ref_);

            // -- Estimate Essential matrix to find the inliers
            vector<DMatch> inlier_matches; // matches, that are inliers
            Mat dummy_R, dummy_t;
            helperEstiMotionByEssential(
                ref_->keypoints_, curr_->keypoints_,
                curr_->matches_, K,
                dummy_R, dummy_t, inlier_matches);
            cout << "Number of inliers by E: " << inlier_matches.size() << endl;
            curr_->inlier_matches_ = inlier_matches;

            // vector<DMatch> &inlier_matches = list_matches[0];
            // curr_->inlier_matches_ = inlier_matches;

            // --  Find the intersection between [DMatches_curr] and [DMatches_prev],
            // --  and 3d-2d correspondance
            vector<Point3f> pts_3d; // a point's 3d pos in cam1 curr_
            vector<Point2f> pts_2d; // a point's 2d pos in image2 pixel curr_
            helperFind3Dto2DCorrespondences(
                curr_->inlier_matches_,
                curr_->keypoints_,
                ref_->inlier_matches_,
                ref_->inliers_pts3d_,
                pts_3d, pts_2d);

            // cout <<"DEBUG 3d-2d cor:"<<endl;
            // int cnt=0;
            // for(auto m:ref_->inlier_matches_)
            //     cout<<cnt++<<" prev curr_: "<<m.trainIdx<<endl;
            // cnt=0;
            // for(auto m:curr_->inlier_matches_)
            //     cout<<cnt++<<"curr curr_: "<<m.queryIdx<<endl;
            cout << "Number of 3d-2d pairs: " << pts_3d.size() << endl;

            // -- Solve PnP, get T_cam1_to_cam2
            Mat R_vec, R, t;
            solvePnPRansac(pts_3d, pts_2d, K, Mat(), R_vec, t, false);
            Rodrigues(R_vec, R);

            // --Triangulate points
            vector<Point3f> pts_3d_in_curr;
            helperTriangulatePoints(
                ref_->keypoints_, curr_->keypoints_,
                curr_->inlier_matches_, R, t, K,
                pts_3d_in_curr);
            curr_->inliers_pts3d_ = pts_3d_in_curr;

            // -- Update current camera pos
            Mat T_curr_to_prev = transRt2T(R, t);
            curr_->T_w_c_ = ref_->T_w_c_ * T_curr_to_prev.inv();
            curr_->R_curr_to_prev_ = R;
            curr_->t_curr_to_prev_ = t;

            //DEBUG
            invRt(R, t);
            cout << "\nprev_to_curr displaycement: " << t.t() << endl;

            // --Update vo state
            vo_state_ = vo_state_; // still OK
            frames_.push_back(curr_);
            ref_=curr_;
        }
        break;
    }

    // Print
    // cout << "R_curr_to_prev_: " << curr_->R_curr_to_prev_ << endl;
    // cout << "t_curr_to_prev_: " << curr_->t_curr_to_prev_ << endl;

    // Save to buff.
    // frames_.push_back(curr_);
    // if (frames_.size() > 10)
    //     frames_.pop_front();
}

} // namespace my_slam