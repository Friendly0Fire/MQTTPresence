#pragma once

#include <string>
#include <debugapi.h>

#include <utility>
#include <deque>

#include "MQTTPresence.h"
#include <mqtt/client.h>

enum class mqtt_status {
    DISCONNECTED = 0,
    CONNECTED = 1,
    DISCONNECTING = 2,
    CONNECTING = 3
};

class mqtt_client {
protected:
    enum qos {
        AT_MOST_ONCE = 0,
        AT_LEAST_ONCE = 1,
        EXACTLY_ONCE = 2
    };

    class result_callback : public virtual mqtt::iaction_listener {
    protected:
        using on_t = std::function<void(const mqtt::token& tok)>;
        on_t failure_, success_;

        void on_failure(const mqtt::token& tok) override {
            failure_(tok);
        }

        void on_success(const mqtt::token& tok) override {
            success_(tok);
        }

    public:
        result_callback(on_t&& success, on_t&& failure)
            : failure_(std::move(failure))
            , success_(std::move(success)) { }
    };

    const int periodic_interval_ = 10;

    const std::string host_, port_, username_, password_, topic_, will_topic_;
    std::string will_content_;
    std::thread periodic_;
    mqtt::async_client_ptr client_;
    std::atomic<mqtt_status> status_ = mqtt_status::DISCONNECTED;
    mutable bool user_active_ = true;
    mutable bool sound_active_ = false;

public:

    mqtt_client(std::string host, std::string port, std::string username,
                std::string password, std::string topic)
        : host_(std::move(host)), port_(std::move(port)), username_(std::move(username)), password_(std::move(password)), topic_(std::move(topic)),
          will_topic_(topic + "/inactive") {}

    ~mqtt_client() {
        if(!client_)
            return;

        disconnect();
    }

    mqtt_status status() const { return status_; }

    void disconnect() {
        if(status_ != mqtt_status::CONNECTED)
            return;

        status_ = mqtt_status::DISCONNECTING;

        if(periodic_.joinable())
            periodic_.join();
        
        user_active(false);
        sound_active(false);

        client_->disconnect(1000)->wait();
        client_.reset();

        status_ = mqtt_status::DISCONNECTED;
    }

    void connect() {
        if(status_ != mqtt_status::DISCONNECTED)
            return;

        status_ = mqtt_status::CONNECTING;

        client_ = std::make_unique<mqtt::async_client>(host_ + ":" + port_, g_unique_identifier);

        mqtt::connect_options connopts;

        if (!username_.empty())
            connopts.set_user_name(username_);
        if (!password_.empty())
            connopts.set_password(password_);

        connopts.set_automatic_reconnect(1000, 30000);

        {
            auto now = std::chrono::system_clock::now();
            auto now_time_t = std::chrono::system_clock::to_time_t(now);
            char buf[1024];
            ctime_s(buf, sizeof(buf), &now_time_t);
            will_content_ = buf;
        }

        mqtt::will_options willopts;
        willopts.set_topic(will_topic_);
        willopts.set_payload(will_content_);
        willopts.set_retained(true);
        willopts.set_qos(qos::AT_LEAST_ONCE);

        connopts.set_will(willopts);

        try {
            client_->connect(connopts)->wait();

            status_ = mqtt_status::CONNECTED;

            user_active(true);
            sound_active(false);

            periodic_ = std::thread([&]() {
                using namespace std::chrono_literals;

                int i = 0;
                while(status_ == mqtt_status::CONNECTED) {
                    if(i >= periodic_interval_) {
                        i = 0;

                        user_active(user_active_);
                        sound_active(sound_active_);
                    }

                    i++;
                    std::this_thread::sleep_for(1s);
                }
            });
        } catch(const mqtt::exception& ex) {
            OutputDebugStringA((std::string("failed to connect: ") + ex.what() + "\n").c_str());
            if(periodic_.joinable())
                periodic_.join();

            client_.reset();
            status_ = mqtt_status::DISCONNECTED;
        }
    }

    void user_active(bool state) const {
        user_active_ = state;
        if(status_ != mqtt_status::CONNECTED)
            return;

        OutputDebugStringA((std::string("user_active = ") + (state ? "true" : "false") + "\n").c_str());

        try {
            client_->publish(topic_ + "/user_active", state ? "true" : "false", qos::AT_LEAST_ONCE, true);
        } catch(const mqtt::exception& ex) {
            OutputDebugStringA((std::string("failed: ") + ex.what() + "\n").c_str());
        }
    }

    void sound_active(bool state) const {
        sound_active_ = state;
        if(status_ != mqtt_status::CONNECTED)
            return;

        OutputDebugStringA((std::string("sound_active = ") + (state ? "true" : "false") + "\n").c_str());

        try {
            client_->publish(topic_ + "/sound_active", state ? "true" : "false", qos::AT_LEAST_ONCE, true);
        } catch(const mqtt::exception& ex) {
            OutputDebugStringA((std::string("failed: ") + ex.what() + "\n").c_str());
        }
    }
};