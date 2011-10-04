/*
* Software License Agreement (BSD License)
*
* Copyright (c) 2011, Willow Garage, Inc.
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions
* are met:
*
* * Redistributions of source code must retain the above copyright
*    notice, this list of conditions and the following disclaimer.
* * Redistributions in binary form must reproduce the above
*    copyright notice, this list of conditions and the following
*    disclaimer in the documentation and/or other materials provided
*    with the distribution.
* * Neither the name of Willow Garage, Inc. nor the names of its
*    contributors may be used to endorse or promote prducts derived
*    from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
* FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
* COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
* CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
* LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
* ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
* POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef ROS_NODE_HANDLE_H_
#define ROS_NODE_HANDLE_H_

#include "../std_msgs/Time.h"
#include "../rosserial_msgs/TopicInfo.h"
#include "../rosserial_msgs/Log.h"
#include "../rosserial_msgs/RequestParam.h"

#define SYNC_SECONDS        5

#define STATE_FIRST_FF       0
#define STATE_SECOND_FF      1
#define STATE_TOPIC_L        2   // waiting for topic id
#define STATE_TOPIC_H        3
#define STATE_SIZE_L         4   // waiting for message size
#define STATE_SIZE_H         5
#define STATE_MESSAGE        6
#define STATE_CHECKSUM       7

#define MSG_TIMEOUT 20  // 20 ms to recieve all of message data

#include "node_output.h"

#include "publisher.h"
#include "msg_receiver.h"
#include "subscriber.h"
#include "rosserial_ids.h"
#include "service_server.h"

namespace ros {

  using rosserial_msgs::TopicInfo;

  // Node Handle
  template<class Hardware,
           int MAX_SUBSCRIBERS=25,
           int MAX_PUBLISHERS=25,
           int INPUT_SIZE=512,
           int OUTPUT_SIZE=512>
  class NodeHandle_ {
    protected:
      Hardware hardware_;
      NodeOutput<Hardware, OUTPUT_SIZE> node_output_;

      // time used for syncing
      unsigned long remote_time_;

      // used for computing current time
      unsigned long sec_offset, nsec_offset;

      unsigned char message_in[INPUT_SIZE];

      Publisher* publishers[MAX_PUBLISHERS];
      MsgReceiver* receivers[MAX_SUBSCRIBERS];

    // Setup Functions
    public:
      NodeHandle_() : node_output_(&hardware_) {}

      Hardware* getHardware() {
        return &hardware_;
      }

      // Start serial, initialize buffers
      void initNode() {
        hardware_.init();
        total_receivers_ = 0;
        reset();
      }

    protected:
      // State machine variables for spinOnce
      int state_;
      int remaining_data_bytes_;
      int topic_;
      int data_index_;
      int checksum_;

      int total_receivers_;

      // used for syncing the time
      unsigned long last_sync_time;
      unsigned long last_sync_receive_time;
      unsigned long last_msg_timeout_time;

      bool registerReceiver(MsgReceiver* receiver) {
        if (total_receivers_ >= MAX_SUBSCRIBERS) {
          return false;
        }
        receivers[total_receivers_] = receiver;
        receiver->id_ = 100 + total_receivers_;
        total_receivers_++;
        return true;
      }

      // Reset state
      void reset() {
        state_ = 0;
        remaining_data_bytes_ = 0;
        topic_ = 0;
        data_index_ = 0;
        checksum_ = 0;
      }

    public:
      // This function goes in your loop() function, it handles
      // serial input and callbacks for subscribers.
      virtual void spinOnce() {
        // restart if timed out
        unsigned long current_time = hardware_.time();  // ms
        // TODO(damonkohler): Why *2200?
        if ((current_time - last_sync_receive_time) > (SYNC_SECONDS * 2200)) {
          node_output_.setConfigured(false);
        }
        // Reset state if message has timed out.
        if (state_ != STATE_FIRST_FF && current_time > last_msg_timeout_time) {
          state_ = STATE_FIRST_FF;
        }

        // while available buffer, read data
        while (true) {
          int inputByte = hardware_.read();
          if(inputByte < 0) {
            // No data available to read.
            break;
          }
          checksum_ += inputByte;
          // TODO(damonkohler): Use switch statement?
          if (state_ == STATE_MESSAGE) {  // message data being recieved
            message_in[data_index_++] = inputByte;
            remaining_data_bytes_--;
            if (remaining_data_bytes_ == 0) {  // is message complete? if so, checksum
              state_ = STATE_CHECKSUM;
            }
          } else if (state_ == STATE_FIRST_FF) {
            if (inputByte == 0xff) {
              state_++;
              last_msg_timeout_time = current_time + MSG_TIMEOUT;
            }
          } else if (state_ == STATE_SECOND_FF) {
            if (inputByte == 0xff) {
              state_++;
            } else {
              state_ = STATE_FIRST_FF;
            }
          } else if (state_ == STATE_TOPIC_L) {  // bottom half of topic id
            topic_ = inputByte;
            state_++;
            checksum_ = inputByte;  // first byte included in checksum
          } else if (state_ == STATE_TOPIC_H) {  // top half of topic id
            topic_ += inputByte << 8;
            state_++;
          } else if (state_ == STATE_SIZE_L) {  // bottom half of message size
            remaining_data_bytes_ = inputByte;
            data_index_ = 0;
            state_++;
          } else if (state_ == STATE_SIZE_H) {  // top half of message size
            remaining_data_bytes_ += inputByte << 8;
            state_ = STATE_MESSAGE;
            if (remaining_data_bytes_ == 0) {
              state_ = STATE_CHECKSUM;
            }
          } else if (state_ == STATE_CHECKSUM) {  // do checksum
            if ((checksum_ % 256) == 255) {
              if (topic_ == TOPIC_NEGOTIATION) {
                requestSyncTime();
                negotiateTopics();
                last_sync_time = current_time;
                last_sync_receive_time = current_time;
              } else if (topic_ == TopicInfo::ID_TIME) {
                syncTime(message_in);
              } else if (topic_ == TopicInfo::ID_PARAMETER_REQUEST) {
                req_param_resp.deserialize(message_in);
                param_recieved = true;
              } else if (receivers[topic_-100] != 0) {
                receivers[topic_-100]->receive(message_in);
              }
            }
            state_ = STATE_FIRST_FF;
          }
        }

        // occasionally sync time
        // TODO(damonkohler): Why *500?
        if (node_output_.configured() &&
            ((current_time - last_sync_time) > (SYNC_SECONDS * 500))) {
          requestSyncTime();
          last_sync_time = current_time;
        }
      }

      // Are we connected to the PC?
      bool connected() {
        return node_output_.configured();
      };

      // Time functions
      void requestSyncTime() {
        std_msgs::Time time;
        node_output_.publish(rosserial_msgs::TopicInfo::ID_TIME, &time);
        remote_time_ = hardware_.time();
      }

      void syncTime(unsigned char* data) {
        std_msgs::Time time;
        unsigned long offset = hardware_.time() - remote_time_;
        time.deserialize(data);
        time.data.sec += offset / 1000;
        time.data.nsec += (offset % 1000) * 1000000UL;
        this->setNow(time.data);
        last_sync_receive_time = hardware_.time();
      }

      Time now() {
        unsigned long ms = hardware_.time();
        Time current_time;
        current_time.sec = ms / 1000 + sec_offset;
        current_time.nsec = (ms % 1000) * 1000000UL + nsec_offset;
        normalizeSecNSec(current_time.sec, current_time.nsec);
        return current_time;
      }

      void setNow(Time& new_now) {
        unsigned long ms = hardware_.time();
        sec_offset = new_now.sec - ms / 1000 - 1;
        nsec_offset = new_now.nsec - (ms % 1000) * 1000000UL + 1000000000UL;
        normalizeSecNSec(sec_offset, nsec_offset);
      }

      // Registration
      bool advertise(Publisher& publisher) {
        // TODO(damonkohler): Pull out a publisher registry or keep track of
        // the next available ID.
        for (int i = 0; i < MAX_PUBLISHERS; i++) {
          if (publishers[i] == 0) {  // empty slot
            publishers[i] = &publisher;
            publisher.id_ = i + 100 + MAX_SUBSCRIBERS;
            publisher.node_output_ = &this->node_output_;
            return true;
          }
        }
        return false;
      }

      // Register a subscriber with the node
      template<typename MsgT>
        bool subscribe(Subscriber<MsgT> &s) {
        return registerReceiver((MsgReceiver*) &s);
      }

      template<typename SrvReq, typename SrvResp>
      bool advertiseService(ServiceServer<SrvReq, SrvResp>& srv) {
        srv.node_output_ = &node_output_;
        return registerReceiver((MsgReceiver*) &srv);
      }

      void negotiateTopics() {
        node_output_.setConfigured(true);
        rosserial_msgs::TopicInfo ti;
        for (int i = 0; i < MAX_PUBLISHERS; i++) {
          if (publishers[i] != 0) {  // non-empty slot
            ti.topic_id = publishers[i]->id_;
            ti.topic_name = (char*) publishers[i]->topic_;
            ti.message_type = (char*) publishers[i]->msg_->getType();
            node_output_.publish(TOPIC_PUBLISHERS, &ti);
          } else {
            // Slots are allocated sequentially and contiguously. We can break
            // out early.
            break;
          }
        }
        for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
          if (receivers[i] != 0) {  // non-empty slot
            ti.topic_id = receivers[i]->id_;
            ti.topic_name = (char*) receivers[i]->topic_;
            ti.message_type = (char*) receivers[i]->getMsgType();
            node_output_.publish(TOPIC_SUBSCRIBERS, &ti);
          } else {
            // Slots are allocated sequentially and contiguously. We can break
            // out early.
            break;
        }
      }

    // Logging
    private:
      void log(char byte, const char* msg) {
        rosserial_msgs::Log l;
        l.level= byte;
        l.msg = (char*)msg;
        this->node_output_.publish(rosserial_msgs::TopicInfo::ID_LOG, &l);
      }

    public:
      void logdebug(const char* msg) {
        log(rosserial_msgs::Log::DEBUG, msg);
      }
      void loginfo(const char* msg) {
        log(rosserial_msgs::Log::INFO, msg);
      }
      void logwarn(const char* msg) {
        log(rosserial_msgs::Log::WARN, msg);
      }
      void logerror(const char* msg) {
        log(rosserial_msgs::Log::ERROR, msg);
      }
      void logfatal(const char* msg) {
        log(rosserial_msgs::Log::FATAL, msg);
      }

    // Retrieve Parameters
    private:
      bool param_recieved;
      rosserial_msgs::RequestParamResponse req_param_resp;

      bool requestParam(const char* name, int time_out =  1000) {
        param_recieved = false;
        rosserial_msgs::RequestParamRequest req;
        req.name  = (char*)name;
        node_output_.publish(TopicInfo::ID_PARAMETER_REQUEST, &req);
        int end_time = hardware_.time();
        while(!param_recieved) {
          spinOnce();
          if (end_time > hardware_.time()) return false;
        }
        return true;
      }

    public:
      bool getParam(const char* name, int* param, int length =1) {
        if (requestParam(name)) {
          if (length == req_param_resp.ints_length) {
            //copy it over
            for(int i=0; i<length; i++)
              param[i] = req_param_resp.ints[i];
            return true;
          }
        }
        return false;
      }
      bool getParam(const char* name, float* param, int length=1) {
        if (requestParam(name)) {
          if (length == req_param_resp.floats_length) {
            //copy it over
            for(int i=0; i<length; i++)
              param[i] = req_param_resp.floats[i];
            return true;
          }
        }
        return false;
      }
      bool getParam(const char* name, char** param, int length=1) {
        if (requestParam(name)) {
          if (length == req_param_resp.strings_length) {
            //copy it over
            for(int i=0; i<length; i++)
              strcpy(param[i],req_param_resp.strings[i]);
            return true;
          }
        }
        return false;
      }
  };

}

#endif
