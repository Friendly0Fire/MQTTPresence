#pragma once

#include "MQTTPresence.h"
#include <mqtt_client_cpp.hpp>

class mqtt_client {
public:
    using inner_client_t = decltype(MQTT_NS::make_sync_client(std::declval<boost::asio::io_context&>(), std::string(), std::string(), MQTT_NS::protocol_version::v3_1_1));

    mqtt_client(const std::string& host, const std::string& port, const std::string& username,
                const std::string& password, const std::string& topic)
        : host_(host), port_(port), username_(username), password_(password), topic_(topic),
          will_topic_(topic + "/inactive") {}

    ~mqtt_client() {
        if(!client_)
            return;

        disconnect();
    }

    void disconnect() {
        disconnecting_ = true;
        periodic_thread_.join();
        
        user_active(false);
        sound_active(false);

        ioc_.stop();
        thread_.join();

        client_.reset();
    }

    void connect() {
        using namespace MQTT_NS;

        client_ = make_sync_client(ioc_, host_, port_);

        client_->set_client_id(g_unique_identifier);
        if (!username_.empty())
            client_->set_user_name(username_);
        if (!password_.empty())
            client_->set_password(password_);
        client_->set_clean_session(true);

        client_->set_connack_handler(
            [this](bool sp, MQTT_NS::connect_return_code connack_return_code) {
                if(!loaded_ && client_) {
                    user_active(true);
                    sound_active(false);
                    loaded_ = true;
                }
                return true;
            }
        );

        {
            auto now = std::chrono::system_clock::now();
            auto now_time_t = std::chrono::system_clock::to_time_t(now);
            char buf[1024];
            ctime_s(buf, sizeof(buf), &now_time_t);
            will_content_ = buf;
        }

        will will(buffer(string_view(will_topic_)), ""_mb, true, MQTT_NS::qos::at_least_once);
        client_->set_will(will);

        client_->connect();
        
        thread_ = std::thread([&]() {
            ioc_.run();
        });

        periodic_thread_ = std::thread([&]() {
            using namespace std::chrono_literals;
            while(!disconnecting_) {
                for(int i = 0; i < 10 && !disconnecting_; i++)
                    std::this_thread::sleep_for(1s);

                user_active(user_active_);
                sound_active(sound_active_);
            }
        });
    }

    void user_active(bool state) const {
        user_active_ = state;
        client_->publish(topic_ + "/user_active", state ? "true" : "false", MQTT_NS::qos::at_least_once, true);
        OutputDebugStringA((std::string("user_active = ") + (state ? "true" : "false") + "\n").c_str());
    }

    void sound_active(bool state) const {
        sound_active_ = state;
        client_->publish(topic_ + "/sound_active", state ? "true" : "false", MQTT_NS::qos::at_least_once, true);
        OutputDebugStringA((std::string("sound_active = ") + (state ? "true" : "false") + "\n").c_str());
    }

protected:
    boost::asio::io_context ioc_;
    const std::string host_, port_, username_, password_, topic_, will_topic_;
    std::string will_content_;
    inner_client_t client_;
    std::thread thread_, periodic_thread_;
    bool loaded_ = false, disconnecting_ = false;
    mutable bool user_active_ = true;
    mutable bool sound_active_ = false;
};