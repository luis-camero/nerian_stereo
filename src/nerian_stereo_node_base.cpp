/*******************************************************************************
 * Copyright (c) 2019 Nerian Vision GmbH
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *******************************************************************************/

#include "nerian_stereo_node_base.h"

namespace nerian_stereo {

void StereoNodeBase::dynamicReconfigureCallback(nerian_stereo::NerianStereoConfig &config, uint32_t level) {
    if (initialConfigReceived) {
        ROS_INFO("Received a new configuration via dynamic_reconfigure");
        // Unfortunately, we have to check for each potential change (no configuration deltas provided).
        // This is done in the autogenerated external code.
        autogen_dynamicReconfigureCallback(config, level);
    } else {
        initialConfigReceived = true;
    }
    lastKnownConfig = config;
}

void StereoNodeBase::updateParameterServerFromDevice(std::map<std::string, ParameterInfo>& cfg) {
    // Publish the current config to the parameter server
    autogen_updateParameterServerFromDevice(cfg);
    // Publish reboot flag to definitely be set to false in the parameter server
    getNH().setParam("/nerian_stereo/reboot", false);
}

void StereoNodeBase::updateDynamicReconfigureFromDevice(std::map<std::string, ParameterInfo>& cfg) {
    autogen_updateDynamicReconfigureFromDevice(cfg);
}
/*
 * \brief Initialize and publish configuration with a dynamic_reconfigure server
 */
void StereoNodeBase::initDynamicReconfigure() {
    std::map<std::string, ParameterInfo> ssParams;
    // Connect to parameter server on device
    ROS_INFO("Connecting to %s for parameter service", remoteHost.c_str());
    try {
        deviceParameters.reset(new DeviceParameters(remoteHost.c_str()));
    } catch(visiontransfer::ParameterException& e) {
        ROS_ERROR("ParameterException while connecting to parameter service: %s", e.what());
        throw;
    }
    try {
        ssParams = deviceParameters->getAllParameters();
    } catch(visiontransfer::TransferException& e) {
        ROS_ERROR("TransferException while obtaining parameter enumeration: %s", e.what());
        throw;
    } catch(visiontransfer::ParameterException& e) {
        ROS_ERROR("ParameterException while obtaining parameter enumeration: %s", e.what());
        throw;
    }
    // First make sure that the parameter server gets all *current* values
    updateParameterServerFromDevice(ssParams);
    // Initialize (and publish) initial configuration from compile-time generated header
    dynReconfServer.reset(new dynamic_reconfigure::Server<nerian_stereo::NerianStereoConfig>());
    // Obtain and publish the default, min, and max values from the device to dyn_reconf
    updateDynamicReconfigureFromDevice(ssParams);
    // Callback for future changes requested from the ROS side
    dynReconfServer->setCallback(boost::bind(&StereoNodeBase::dynamicReconfigureCallback, this, _1, _2));
}

/**
 * \brief Performs general initializations
 */
void StereoNodeBase::init() {
    ros::NodeHandle& privateNh = getPrivateNH();

    // Read all ROS parameters
    std::string intensityChannel = "mono8";
    privateNh.getParam("point_cloud_intensity_channel", intensityChannel);
    if(intensityChannel == "none") {
        pointCloudColorMode = NONE;
    } else if(intensityChannel == "rgb8") {
        pointCloudColorMode = RGB_COMBINED;
    } else if(intensityChannel == "rgb32f") {
        pointCloudColorMode = RGB_SEPARATE;
    } else {
        pointCloudColorMode = INTENSITY;
    }

    if (!privateNh.getParam("color_code_disparity_map", colorCodeDispMap)) {
        colorCodeDispMap = "";
    }

    if (!privateNh.getParam("color_code_legend", colorCodeLegend)) {
        colorCodeLegend = false;
    }

    if (!privateNh.getParam("top_level_frame", frame)) {
        if (!privateNh.getParam("frame", frame)) {
            frame = "world";
        }
    }

    if (!privateNh.getParam("internal_frame", internalFrame)) {
        internalFrame = "nerian_stereo";
    }

    if (!privateNh.getParam("remote_port", remotePort)) {
        remotePort = "7681";
    }

    if (!privateNh.getParam("remote_host", remoteHost)) {
        remoteHost = "0.0.0.0";
    }

    if (!privateNh.getParam("use_tcp", useTcp)) {
        useTcp = false;
    }

    if (!privateNh.getParam("ros_coordinate_system", rosCoordinateSystem)) {
        rosCoordinateSystem = true;
    }

    if (!privateNh.getParam("ros_timestamps", rosTimestamps)) {
        rosTimestamps = true;
    }

    if (!privateNh.getParam("calibration_file", calibFile)) {
        calibFile = "";
    }

    if (!privateNh.getParam("delay_execution", execDelay)) {
        execDelay = 0;
    }

    if (!privateNh.getParam("max_depth", maxDepth)) {
        maxDepth = -1;
    }

    if (!privateNh.getParam("q_from_calib_file", useQFromCalibFile)) {
        useQFromCalibFile = false;
    }

    // Apply an initial delay if configured
    ros::Duration(execDelay).sleep();

    // Create publishers
    disparityPublisher.reset(new ros::Publisher(getNH().advertise<sensor_msgs::Image>(
        "/nerian_stereo/disparity_map", 5)));
    leftImagePublisher.reset(new ros::Publisher(getNH().advertise<sensor_msgs::Image>(
        "/nerian_stereo/left_image", 5)));
    rightImagePublisher.reset(new ros::Publisher(getNH().advertise<sensor_msgs::Image>(
        "/nerian_stereo/right_image", 5)));

    loadCameraCalibration();

    cameraInfoPublisher.reset(new ros::Publisher(getNH().advertise<nerian_stereo::StereoCameraInfo>(
        "/nerian_stereo/stereo_camera_info", 1)));
    cloudPublisher.reset(new ros::Publisher(getNH().advertise<sensor_msgs::PointCloud2>(
        "/nerian_stereo/point_cloud", 5)));

    transformBroadcaster.reset(new tf2_ros::TransformBroadcaster());
    currentTransform.header.stamp = ros::Time::now();
    currentTransform.header.frame_id = frame;
    currentTransform.child_frame_id = internalFrame;
    currentTransform.transform.translation.x = 0.0;
    currentTransform.transform.translation.y = 0.0;
    currentTransform.transform.translation.z = 0.0;
    currentTransform.transform.rotation.x = 0.0;
    currentTransform.transform.rotation.y = 0.0;
    currentTransform.transform.rotation.z = 0.0;
    currentTransform.transform.rotation.w = 1.0;

}

void StereoNodeBase::initDataChannelService() {
    dataChannelService.reset(new DataChannelService(remoteHost.c_str()));
}

void StereoNodeBase::prepareAsyncTransfer() {
    ROS_INFO("Connecting to %s:%s for data transfer", remoteHost.c_str(), remotePort.c_str());
    asyncTransfer.reset(new AsyncTransfer(remoteHost.c_str(), remotePort.c_str(),
        useTcp ? ImageProtocol::PROTOCOL_TCP : ImageProtocol::PROTOCOL_UDP));
}

void StereoNodeBase::processOneImageSet() {
    // Receive image data
    ImageSet imageSet;
    if(asyncTransfer->collectReceivedImageSet(imageSet, 0.0)) {

        // Get time stamp
        ros::Time stamp;
        if(rosTimestamps) {
            stamp = ros::Time::now();
        } else {
            int secs = 0, microsecs = 0;
            imageSet.getTimestamp(secs, microsecs);
            stamp = ros::Time(secs, microsecs*1000);
        }

        // Publish image data messages for all images included in the set
        if (imageSet.hasImageType(ImageSet::IMAGE_LEFT)) {
            publishImageMsg(imageSet, imageSet.getIndexOf(ImageSet::IMAGE_LEFT), stamp, false, leftImagePublisher.get());
        }
        if (imageSet.hasImageType(ImageSet::IMAGE_DISPARITY)) {
            publishImageMsg(imageSet, imageSet.getIndexOf(ImageSet::IMAGE_DISPARITY), stamp, true, disparityPublisher.get());
        }
        if (imageSet.hasImageType(ImageSet::IMAGE_RIGHT)) {
            publishImageMsg(imageSet, imageSet.getIndexOf(ImageSet::IMAGE_RIGHT), stamp, false, rightImagePublisher.get());
        }

        if(cloudPublisher->getNumSubscribers() > 0) {
            if(recon3d == nullptr) {
                // First initialize
                initPointCloud();
            }

            publishPointCloudMsg(imageSet, stamp);
        }

        if(cameraInfoPublisher != NULL && cameraInfoPublisher->getNumSubscribers() > 0) {
            publishCameraInfo(stamp, imageSet);
        }

        // Display some simple statistics
        frameNum++;
        if(stamp.sec != lastLogTime.sec) {
            if(lastLogTime != ros::Time()) {
                double dt = (stamp - lastLogTime).toSec();
                double fps = (frameNum - lastLogFrames) / dt;
                ROS_INFO("%.1f fps", fps);
            }
            lastLogFrames = frameNum;
            lastLogTime = stamp;
        }
    }
}

void StereoNodeBase::loadCameraCalibration() {
    if(calibFile == "" ) {
        ROS_WARN("No camera calibration file configured. Cannot publish detailed camera information!");
    } else {
        bool success = false;
        try {
            if (calibStorage.open(calibFile, cv::FileStorage::READ)) {
                success = true;
            }
        } catch(...) {
        }

        if(!success) {
            ROS_WARN("Error reading calibration file: %s\n"
                "Cannot publish detailed camera information!", calibFile.c_str());
        }
    }
}

void StereoNodeBase::publishImageMsg(const ImageSet& imageSet, int imageIndex, ros::Time stamp, bool allowColorCode,
        ros::Publisher* publisher) {

    if(publisher->getNumSubscribers() <= 0) {
        return; //No subscribers
    }

    cv_bridge::CvImage cvImg;
    cvImg.header.frame_id = internalFrame;
    cvImg.header.stamp = stamp;
    cvImg.header.seq = imageSet.getSequenceNumber(); // Actually ROS will overwrite this

    bool format12Bit = (imageSet.getPixelFormat(imageIndex) == ImageSet::FORMAT_12_BIT_MONO);
    string encoding = "";
    bool ok = true;

    if(colorCodeDispMap == "" || colorCodeDispMap == "none" || !allowColorCode || !format12Bit) {
        switch (imageSet.getPixelFormat(imageIndex)) {
            case ImageSet::FORMAT_8_BIT_RGB: {
                cv::Mat rgbImg(imageSet.getHeight(), imageSet.getWidth(),
                    CV_8UC3,
                    imageSet.getPixelData(imageIndex), imageSet.getRowStride(imageIndex));
                cvImg.image = rgbImg;
                encoding = "rgb8";
                break;
            }
            case ImageSet::FORMAT_8_BIT_MONO:
            case ImageSet::FORMAT_12_BIT_MONO: {
                cv::Mat monoImg(imageSet.getHeight(), imageSet.getWidth(),
                    format12Bit ? CV_16UC1 : CV_8UC1,
                    imageSet.getPixelData(imageIndex), imageSet.getRowStride(imageIndex));
                cvImg.image = monoImg;
                encoding = (format12Bit ? "mono16": "mono8");
                break;
            }
            default: {
                ROS_WARN("Omitting an image with unhandled pixel format");
                ok = false;
            }
        }
    } else {
        cv::Mat monoImg(imageSet.getHeight(), imageSet.getWidth(),
            format12Bit ? CV_16UC1 : CV_8UC1,
            imageSet.getPixelData(imageIndex), imageSet.getRowStride(imageIndex));

        if(colCoder == NULL) {
            int dispMin = 0, dispMax = 0;
            imageSet.getDisparityRange(dispMin, dispMax);

            colCoder.reset(new ColorCoder(
                colorCodeDispMap == "rainbow" ? ColorCoder::COLOR_RAINBOW_BGR : ColorCoder::COLOR_RED_BLUE_BGR,
                dispMin*16, dispMax*16, true, true));
            if(colorCodeLegend) {
                // Create the legend
                colDispMap = colCoder->createLegendBorder(monoImg.cols, monoImg.rows, 1.0/16.0);
            } else {
                colDispMap = cv::Mat_<cv::Vec3b>(monoImg.rows, monoImg.cols);
            }
        }

        cv::Mat_<cv::Vec3b> dispSection = colDispMap(cv::Rect(0, 0, monoImg.cols, monoImg.rows));

        colCoder->codeImage(cv::Mat_<unsigned short>(monoImg), dispSection);
        cvImg.image = colDispMap;
        encoding = "bgr8";
    }

    if (ok) {
        sensor_msgs::ImagePtr msg = cvImg.toImageMsg();
        msg->encoding = encoding;
        publisher->publish(msg);
    }
}

void StereoNodeBase::qMatrixToRosCoords(const float* src, float* dst) {
    dst[0] = src[8];   dst[1] = src[9];
    dst[2] = src[10];  dst[3] = src[11];

    dst[4] = -src[0];  dst[5] = -src[1];
    dst[6] = -src[2];  dst[7] = -src[3];

    dst[8] = -src[4];  dst[9] = -src[5];
    dst[10] = -src[6]; dst[11] = -src[7];

    dst[12] = src[12]; dst[13] = src[13];
    dst[14] = src[14]; dst[15] = src[15];
}

void StereoNodeBase::publishPointCloudMsg(ImageSet& imageSet, ros::Time stamp) {
    if ((!imageSet.hasImageType(ImageSet::IMAGE_DISPARITY))
        || (imageSet.getPixelFormat(ImageSet::IMAGE_DISPARITY) != ImageSet::FORMAT_12_BIT_MONO)) {
        return; // This is not a disparity map
    }

    // Set static q matrix if desired
    if(useQFromCalibFile) {
        static std::vector<float> q;
        calibStorage["Q"] >> q;
        imageSet.setQMatrix(&q[0]);
    }

    // Transform Q-matrix if desired
    float qRos[16];
    if(rosCoordinateSystem) {
        qMatrixToRosCoords(imageSet.getQMatrix(), qRos);
        imageSet.setQMatrix(qRos);
    }

    // Get 3D points
    float* pointMap = nullptr;
    try {
        pointMap = recon3d->createPointMap(imageSet, 0);
    } catch(std::exception& ex) {
        cerr << "Error creating point cloud: " << ex.what() << endl;
        return;
    }

    // Create message object and set header
    pointCloudMsg->header.stamp = stamp;
    pointCloudMsg->header.frame_id = internalFrame;
    pointCloudMsg->header.seq = imageSet.getSequenceNumber(); // Actually ROS will overwrite this

    // Copy 3D points
    if(pointCloudMsg->data.size() != imageSet.getWidth()*imageSet.getHeight()*4*sizeof(float)) {
        // Allocate buffer
        pointCloudMsg->data.resize(imageSet.getWidth()*imageSet.getHeight()*4*sizeof(float));

        // Set basic data
        pointCloudMsg->width = imageSet.getWidth();
        pointCloudMsg->height = imageSet.getHeight();
        pointCloudMsg->is_bigendian = false;
        pointCloudMsg->point_step = 4*sizeof(float);
        pointCloudMsg->row_step = imageSet.getWidth() * pointCloudMsg->point_step;
        pointCloudMsg->is_dense = false;
    }

    if(maxDepth < 0) {
        // Just copy everything
        memcpy(&pointCloudMsg->data[0], pointMap,
            imageSet.getWidth()*imageSet.getHeight()*4*sizeof(float));
    } else {
        // Only copy points up to maximum depth
        if(rosCoordinateSystem) {
            copyPointCloudClamped<0>(pointMap, reinterpret_cast<float*>(&pointCloudMsg->data[0]),
                imageSet.getWidth()*imageSet.getHeight());
        } else {
            copyPointCloudClamped<2>(pointMap, reinterpret_cast<float*>(&pointCloudMsg->data[0]),
                imageSet.getWidth()*imageSet.getHeight());
        }
    }

    if (imageSet.hasImageType(ImageSet::IMAGE_LEFT)) {
        // Copy intensity values as well (if we received any image data)
        switch(pointCloudColorMode) {
            case INTENSITY:
                copyPointCloudIntensity<INTENSITY>(imageSet);
                break;
            case RGB_COMBINED:
                copyPointCloudIntensity<RGB_COMBINED>(imageSet);
                break;
            case RGB_SEPARATE:
                copyPointCloudIntensity<RGB_SEPARATE>(imageSet);
                break;
            case NONE:
                break;
        }
    }

    cloudPublisher->publish(pointCloudMsg);
}

template <StereoNodeBase::PointCloudColorMode colorMode> void StereoNodeBase::copyPointCloudIntensity(ImageSet& imageSet) {
    // Get pointers to the beginning and end of the point cloud
    unsigned char* cloudStart = &pointCloudMsg->data[0];
    unsigned char* cloudEnd = &pointCloudMsg->data[0]
        + imageSet.getWidth()*imageSet.getHeight()*4*sizeof(float);

    if(imageSet.getPixelFormat(ImageSet::IMAGE_LEFT) == ImageSet::FORMAT_8_BIT_MONO) {
        // Get pointer to the current pixel and end of current row
        unsigned char* imagePtr = imageSet.getPixelData(ImageSet::IMAGE_LEFT);
        unsigned char* rowEndPtr = imagePtr + imageSet.getWidth();
        int rowIncrement = imageSet.getRowStride(ImageSet::IMAGE_LEFT) - imageSet.getWidth();

        for(unsigned char* cloudPtr = cloudStart + 3*sizeof(float);
                cloudPtr < cloudEnd; cloudPtr+= 4*sizeof(float)) {
            if(colorMode == RGB_SEPARATE) {// RGB as float
                *reinterpret_cast<float*>(cloudPtr) = static_cast<float>(*imagePtr) / 255.0F;
            } else if(colorMode == RGB_COMBINED) {// RGB as integer
                const unsigned char intensity = *imagePtr;
                *reinterpret_cast<unsigned int*>(cloudPtr) = (intensity << 16) | (intensity << 8) | intensity;
            } else {
                *cloudPtr = *imagePtr;
            }

            imagePtr++;
            if(imagePtr == rowEndPtr) {
                // Progress to next row
                imagePtr += rowIncrement;
                rowEndPtr = imagePtr + imageSet.getWidth();
            }
        }
    } else if(imageSet.getPixelFormat(ImageSet::IMAGE_LEFT) == ImageSet::FORMAT_12_BIT_MONO) {
        // Get pointer to the current pixel and end of current row
        unsigned short* imagePtr = reinterpret_cast<unsigned short*>(imageSet.getPixelData(ImageSet::IMAGE_LEFT));
        unsigned short* rowEndPtr = imagePtr + imageSet.getWidth();
        int rowIncrement = imageSet.getRowStride(ImageSet::IMAGE_LEFT) - 2*imageSet.getWidth();

        for(unsigned char* cloudPtr = cloudStart + 3*sizeof(float);
                cloudPtr < cloudEnd; cloudPtr+= 4*sizeof(float)) {

            if(colorMode == RGB_SEPARATE) {// RGB as float
                *reinterpret_cast<float*>(cloudPtr) = static_cast<float>(*imagePtr) / 4095.0F;
            } else if(colorMode == RGB_COMBINED) {// RGB as integer
                const unsigned char intensity = *imagePtr/16;
                *reinterpret_cast<unsigned int*>(cloudPtr) = (intensity << 16) | (intensity << 8) | intensity;
            } else {
                *cloudPtr = *imagePtr/16;
            }

            imagePtr++;
            if(imagePtr == rowEndPtr) {
                // Progress to next row
                imagePtr += rowIncrement;
                rowEndPtr = imagePtr + imageSet.getWidth();
            }
        }
    } else if(imageSet.getPixelFormat(ImageSet::IMAGE_LEFT) == ImageSet::FORMAT_8_BIT_RGB) {
        // Get pointer to the current pixel and end of current row
        unsigned char* imagePtr = imageSet.getPixelData(ImageSet::IMAGE_LEFT);
        unsigned char* rowEndPtr = imagePtr + imageSet.getWidth();
        int rowIncrement = imageSet.getRowStride(ImageSet::IMAGE_LEFT) - imageSet.getWidth();

        static bool warned = false;
        if(colorMode == RGB_SEPARATE && !warned) {
            warned = true;
            ROS_WARN("RGBF32 is not supported for color images. Please use RGB8!");
        }

        for(unsigned char* cloudPtr = cloudStart + 3*sizeof(float);
                cloudPtr < cloudEnd; cloudPtr+= 4*sizeof(float)) {
            if(colorMode == RGB_SEPARATE) {// RGB as float
                *reinterpret_cast<float*>(cloudPtr) = static_cast<float>(imagePtr[2]) / 255.0F;
            } else if(colorMode == RGB_COMBINED) {// RGB as integer
                *reinterpret_cast<unsigned int*>(cloudPtr) = (imagePtr[0] << 16) | (imagePtr[1] << 8) | imagePtr[2];
            } else {
                *cloudPtr = (imagePtr[0] + imagePtr[1]*2 + imagePtr[2])/4;
            }

            imagePtr+=3;
            if(imagePtr == rowEndPtr) {
                // Progress to next row
                imagePtr += rowIncrement;
                rowEndPtr = imagePtr + imageSet.getWidth();
            }
        }
    } else {
        throw std::runtime_error("Invalid pixel format!");
    }
}

template <int coord> void StereoNodeBase::copyPointCloudClamped(float* src, float* dst, int size) {
    // Only copy points that are below the minimum depth
    float* endPtr = src + 4*size;
    for(float *srcPtr = src, *dstPtr = dst; srcPtr < endPtr; srcPtr+=4, dstPtr+=4) {
        if(srcPtr[coord] > maxDepth) {
            dstPtr[0] = std::numeric_limits<float>::quiet_NaN();
            dstPtr[1] = std::numeric_limits<float>::quiet_NaN();
            dstPtr[2] = std::numeric_limits<float>::quiet_NaN();
        } else {
            dstPtr[0] = srcPtr[0];
            dstPtr[1] = srcPtr[1];
            dstPtr[2] = srcPtr[2];
        }
    }
}

void StereoNodeBase::initPointCloud() {
    //ros::NodeHandle privateNh("~"); // RYT TODO check

    // Initialize 3D reconstruction class
    recon3d.reset(new Reconstruct3D);

    // Initialize message
    pointCloudMsg.reset(new sensor_msgs::PointCloud2);

    // Set channel information.
    sensor_msgs::PointField fieldX;
    fieldX.name ="x";
    fieldX.offset = 0;
    fieldX.datatype = sensor_msgs::PointField::FLOAT32;
    fieldX.count = 1;
    pointCloudMsg->fields.push_back(fieldX);

    sensor_msgs::PointField fieldY;
    fieldY.name ="y";
    fieldY.offset = sizeof(float);
    fieldY.datatype = sensor_msgs::PointField::FLOAT32;
    fieldY.count = 1;
    pointCloudMsg->fields.push_back(fieldY);

    sensor_msgs::PointField fieldZ;
    fieldZ.name ="z";
    fieldZ.offset = 2*sizeof(float);
    fieldZ.datatype = sensor_msgs::PointField::FLOAT32;
    fieldZ.count = 1;
    pointCloudMsg->fields.push_back(fieldZ);

    if(pointCloudColorMode == INTENSITY) {
        sensor_msgs::PointField fieldI;
        fieldI.name ="intensity";
        fieldI.offset = 3*sizeof(float);
        fieldI.datatype = sensor_msgs::PointField::UINT8;
        fieldI.count = 1;
        pointCloudMsg->fields.push_back(fieldI);
    }
    else if(pointCloudColorMode == RGB_SEPARATE) {
        sensor_msgs::PointField fieldRed;
        fieldRed.name ="r";
        fieldRed.offset = 3*sizeof(float);
        fieldRed.datatype = sensor_msgs::PointField::FLOAT32;
        fieldRed.count = 1;
        pointCloudMsg->fields.push_back(fieldRed);

        sensor_msgs::PointField fieldGreen;
        fieldGreen.name ="g";
        fieldGreen.offset = 3*sizeof(float);
        fieldGreen.datatype = sensor_msgs::PointField::FLOAT32;
        fieldGreen.count = 1;
        pointCloudMsg->fields.push_back(fieldGreen);

        sensor_msgs::PointField fieldBlue;
        fieldBlue.name ="b";
        fieldBlue.offset = 3*sizeof(float);
        fieldBlue.datatype = sensor_msgs::PointField::FLOAT32;
        fieldBlue.count = 1;
        pointCloudMsg->fields.push_back(fieldBlue);
    } else if(pointCloudColorMode == RGB_COMBINED) {
        sensor_msgs::PointField fieldRGB;
        fieldRGB.name ="rgb";
        fieldRGB.offset = 3*sizeof(float);
        fieldRGB.datatype = sensor_msgs::PointField::UINT32;
        fieldRGB.count = 1;
        pointCloudMsg->fields.push_back(fieldRGB);
    }
}

void StereoNodeBase::publishCameraInfo(ros::Time stamp, const ImageSet& imageSet) {
    if(camInfoMsg == NULL) {
        // Initialize the camera info structure
        camInfoMsg.reset(new nerian_stereo::StereoCameraInfo);

        camInfoMsg->header.frame_id = internalFrame;
        camInfoMsg->header.seq = imageSet.getSequenceNumber(); // Actually ROS will overwrite this

        if(calibFile != "") {
            std::vector<int> sizeVec;
            calibStorage["size"] >> sizeVec;
            if(sizeVec.size() != 2) {
                std::runtime_error("Calibration file format error!");
            }

            camInfoMsg->left_info.header = camInfoMsg->header;
            camInfoMsg->left_info.width = sizeVec[0];
            camInfoMsg->left_info.height = sizeVec[1];
            camInfoMsg->left_info.distortion_model = "plumb_bob";
            calibStorage["D1"] >> camInfoMsg->left_info.D;
            readCalibrationArray("M1", camInfoMsg->left_info.K);
            readCalibrationArray("R1", camInfoMsg->left_info.R);
            readCalibrationArray("P1", camInfoMsg->left_info.P);
            camInfoMsg->left_info.binning_x = 1;
            camInfoMsg->left_info.binning_y = 1;
            camInfoMsg->left_info.roi.do_rectify = false;
            camInfoMsg->left_info.roi.height = 0;
            camInfoMsg->left_info.roi.width = 0;
            camInfoMsg->left_info.roi.x_offset = 0;
            camInfoMsg->left_info.roi.y_offset = 0;

            camInfoMsg->right_info.header = camInfoMsg->header;
            camInfoMsg->right_info.width = sizeVec[0];
            camInfoMsg->right_info.height = sizeVec[1];
            camInfoMsg->right_info.distortion_model = "plumb_bob";
            calibStorage["D2"] >> camInfoMsg->right_info.D;
            readCalibrationArray("M2", camInfoMsg->right_info.K);
            readCalibrationArray("R2", camInfoMsg->right_info.R);
            readCalibrationArray("P2", camInfoMsg->right_info.P);
            camInfoMsg->right_info.binning_x = 1;
            camInfoMsg->right_info.binning_y = 1;
            camInfoMsg->right_info.roi.do_rectify = false;
            camInfoMsg->right_info.roi.height = 0;
            camInfoMsg->right_info.roi.width = 0;
            camInfoMsg->right_info.roi.x_offset = 0;
            camInfoMsg->right_info.roi.y_offset = 0;

            readCalibrationArray("Q", camInfoMsg->Q);
            readCalibrationArray("T", camInfoMsg->T_left_right);
            readCalibrationArray("R", camInfoMsg->R_left_right);
        }
    }

    double dt = (stamp - lastCamInfoPublish).toSec();
    if(dt > 1.0) {
        // Rather use the Q-matrix that we received over the network if it is valid
        const float* qMatrix = imageSet.getQMatrix();
        if(qMatrix[0] != 0.0) {
            for(int i=0; i<16; i++) {
                camInfoMsg->Q[i] = static_cast<double>(qMatrix[i]);
            }
        }

        // Publish once per second
        camInfoMsg->header.stamp = stamp;
        camInfoMsg->left_info.header.stamp = stamp;
        camInfoMsg->right_info.header.stamp = stamp;
        cameraInfoPublisher->publish(camInfoMsg);

        lastCamInfoPublish = stamp;
    }
}

template<class T> void StereoNodeBase::readCalibrationArray(const char* key, T& dest) {
    std::vector<double> doubleVec;
    calibStorage[key] >> doubleVec;

    if(doubleVec.size() != dest.size()) {
        std::runtime_error("Calibration file format error!");
    }

    std::copy(doubleVec.begin(), doubleVec.end(), dest.begin());
}

void StereoNodeBase::processDataChannels() {
    auto now = ros::Time::now();
    if ((now - currentTransform.header.stamp).toSec() < 0.01) {
        // Limit to 100 Hz transform update frequency
        return;
    }
    if (dataChannelService->imuAvailable()) {
        // Obtain and publish the most recent orientation
        TimestampedQuaternion tsq = dataChannelService->imuGetRotationQuaternion();
        currentTransform.header.stamp = now;
        if(rosCoordinateSystem) {
            currentTransform.transform.rotation.x = tsq.x();
            currentTransform.transform.rotation.y = -tsq.z();
            currentTransform.transform.rotation.z = tsq.y();
        } else {
            currentTransform.transform.rotation.x = tsq.x();
            currentTransform.transform.rotation.y = tsq.y();
            currentTransform.transform.rotation.z = tsq.z();
        }
        currentTransform.transform.rotation.w = tsq.w();

        /*
        // DEBUG: Quaternion->Euler + debug output
        double roll, pitch, yaw;
        tf2::Quaternion q(tsq.x(), rosCoordinateSystem?(-tsq.z()):tsq.y(), rosCoordinateSystem?tsq.y():tsq.z(), tsq.w());
        tf2::Matrix3x3 m(q);
        m.getRPY(roll, pitch, yaw);
        std::cout << "Orientation:" << std::setprecision(2) << std::fixed << " Roll " << (180.0*roll/M_PI) << " Pitch " << (180.0*pitch/M_PI) << " Yaw " << (180.0*yaw/M_PI) << std::endl;
        */

        publishTransform();
    } else {
        // We must periodically republish due to ROS interval constraints
        /*
        // DEBUG: Impart a (fake) periodic horizontal swaying motion
        static double DEBUG_t = 0.0;
        DEBUG_t += 0.1;
        tf2::Quaternion q;
        q.setRPY(0, 0, 0.3*sin(DEBUG_t));
        currentTransform.header.stamp = ros::Time::now();
        currentTransform.transform.rotation.x = q.x();
        currentTransform.transform.rotation.y = q.y();
        currentTransform.transform.rotation.z = q.z();
        currentTransform.transform.rotation.w = q.w();
        */
        currentTransform.header.stamp = now;
        publishTransform();
    }
}

void StereoNodeBase::publishTransform() {
    transformBroadcaster->sendTransform(currentTransform);
}

} // namespace
