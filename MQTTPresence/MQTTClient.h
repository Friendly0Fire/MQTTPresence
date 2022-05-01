#pragma once

#include <string>
#include <debugapi.h>

#include <utility>
#include <deque>

#include "MQTTPresence.h"
#include <mqtt/client.h>

extern bool g_user_active;
extern bool g_sound_active;

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

    const qos default_qos_ = qos::EXACTLY_ONCE;

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

    const std::string host_, port_, username_, password_, devicename_;
    std::string will_content_;
    std::thread periodic_;
    mqtt::async_client_ptr client_;
    std::atomic<mqtt_status> status_ = mqtt_status::DISCONNECTED;

    std::string base_topic() const { return "homeassistant/binary_sensor/" + devicename_; }

    void broadcast_home_assistant_config(const std::string& name) {
	    
        auto ha_cfg = base_topic() + "_" + name + "/config";
        auto ha_cfg_contents = "{\"name\": \"" + devicename_ + "_" + name + "\", \"device_class\": \"presence\", \"state_topic\": \"" + base_topic() + "_" + name + "/state\"}";
        client_->publish(ha_cfg.c_str(), ha_cfg_contents.c_str(), ha_cfg_contents.size(), default_qos_, true);
    }

public:

    mqtt_client(std::string host, std::string port, std::string username,
                std::string password, std::string devicename)
        : host_(std::move(host)), port_(std::move(port)), username_(std::move(username)), password_(std::move(password)), devicename_(std::move(devicename)) {}

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

#ifdef _DEBUG
        OutputDebugStringA("Disconnecting...\n");
#endif

        if(periodic_.joinable())
            periodic_.join();

#ifdef _DEBUG
        OutputDebugStringA("Periodic thread joined...\n");
#endif
        
        user_active(false);
        sound_active(false);

#ifdef _DEBUG
        OutputDebugStringA("Activity messages sent...\n");
#endif
        try
        {
            client_->disconnect(1000)->wait();
        }
        catch (mqtt::exception ex)
        {
#ifdef _DEBUG
            OutputDebugStringA(std::format("Disconnect exception: {}", ex.to_string().c_str()).c_str());
#endif
        }

#ifdef _DEBUG
        OutputDebugStringA("Disconnection processed...\n");
#endif

        client_.reset();

        status_ = mqtt_status::DISCONNECTED;

#ifdef _DEBUG
        OutputDebugStringA("Client destroyed.\n");
#endif
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
            mqtt::will_options willopts;
            willopts.set_topic(base_topic() + "_disconnected/state");
            willopts.set_payload(mqtt::string("true"));
            willopts.set_retained(false);
            willopts.set_qos(default_qos_);

            connopts.set_will(std::move(willopts));
        }

        try {
            client_->connect(connopts)->wait();
            status_ = mqtt_status::CONNECTED;
            
            broadcast_home_assistant_config("user");
            broadcast_home_assistant_config("sound");
            broadcast_home_assistant_config("disconnected");
            client_->publish(base_topic() + "_disconnected/state", mqtt::string("false"), qos::EXACTLY_ONCE, true);

            user_active(true);
            sound_active(false);

            periodic_ = std::thread([&]() {
                using namespace std::chrono_literals;

                int i = 0;
                while(status_ == mqtt_status::CONNECTED) {
                    if(i >= periodic_interval_) {
                        i = 0;

                        user_active();
                        sound_active();
                    }

                    i++;
                    std::this_thread::sleep_for(1s);
                }
            });
        } catch(const mqtt::exception& ex) {
#ifdef _DEBUG
            OutputDebugStringA((std::string("failed to connect: ") + ex.what() + "\n").c_str());
#endif
            if(periodic_.joinable())
                periodic_.join();

            client_.reset();
            status_ = mqtt_status::DISCONNECTED;
        }
    }

    void user_active(bool state = g_user_active) const {
        if(status_ == mqtt_status::DISCONNECTED)
            return;

#ifdef _DEBUG
        OutputDebugStringA((std::string("user_active = ") + (state ? "true" : "false") + "\n").c_str());
#endif

        try {
            client_->publish(base_topic() + "_user/state", state ? "ON" : "OFF", default_qos_, false)->wait();
        } catch(const mqtt::exception& ex) {
#ifdef _DEBUG
            OutputDebugStringA((std::string("failed: ") + ex.what() + "\n").c_str());
#endif
        }
    }

    void sound_active(bool state = g_sound_active) const {
        if (status_ == mqtt_status::DISCONNECTED)
            return;

#ifdef _DEBUG
        OutputDebugStringA((std::string("sound_active = ") + (state ? "true" : "false") + "\n").c_str());
#endif

        try {
            client_->publish(base_topic() + "_sound/state", state ? "ON" : "OFF", default_qos_, false)->wait();
        } catch(const mqtt::exception& ex) {
#ifdef _DEBUG
            OutputDebugStringA((std::string("failed: ") + ex.what() + "\n").c_str());
#endif
        }
    }
};