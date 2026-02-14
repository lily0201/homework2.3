#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/int64.hpp"

#include "rm_server/msg/get_el_gamal_params.hpp"
#include "rm_server/srv/el_gamal_encrypt.hpp"

using namespace std::chrono_literals;

class ElGamalClient : public rclcpp::Node
{
public:
  ElGamalClient()
  : Node("elgamal_client"),
    rng_(std::random_device{}())
  {
    params_sub_ = this->create_subscription<rm_server::msg::GetElGamalParams>(
      "elgamal_params", 10,
      std::bind(&ElGamalClient::on_params, this, std::placeholders::_1));

    result_pub_ = this->create_publisher<std_msgs::msg::Int64>("elgamal_result", 10);
    encrypt_client_ = this->create_client<rm_server::srv::ElGamalEncrypt>("elgamal_service");

    RCLCPP_INFO(this->get_logger(), "ElGamal client started.");
  }

private:
  static uint64_t mod_mul(uint64_t a, uint64_t b, uint64_t mod)
  {
    return static_cast<uint64_t>((static_cast<__uint128_t>(a) * b) % mod);
  }

  static uint64_t mod_pow(uint64_t base, uint64_t exp, uint64_t mod)
  {
    uint64_t result = 1 % mod;
    base %= mod;

    while (exp > 0) {
      if (exp & 1U) {
        result = mod_mul(result, base, mod);
      }
      base = mod_mul(base, base, mod);
      exp >>= 1U;
    }

    return result;
  }

  void on_params(const rm_server::msg::GetElGamalParams::SharedPtr msg)
  {
    if (finished_ || waiting_service_) {
      return;
    }

    if (round_ >= kTotalRounds) {
      finished_ = true;
      RCLCPP_INFO(this->get_logger(), "All %d rounds are complete.", kTotalRounds);
      return;
    }

    const uint64_t p = msg->p;
    const uint64_t a = msg->a;

    if (p < 3U) {
      RCLCPP_ERROR(this->get_logger(), "Invalid p=%lu", p);
      return;
    }

    std::uniform_int_distribution<uint64_t> dist(1, p - 2);
    const uint64_t n = dist(rng_);
    const uint64_t b = mod_pow(a, n, p);

    if (!encrypt_client_->wait_for_service(1s)) {
      RCLCPP_WARN(this->get_logger(), "Service elgamal_service is not available yet.");
      return;
    }

    auto request = std::make_shared<rm_server::srv::ElGamalEncrypt::Request>();
    request->public_key = b;
    waiting_service_ = true;

    RCLCPP_INFO(
      this->get_logger(),
      "[Round %d] p=%lu a=%lu n=%lu b=%lu, calling elgamal_service",
      round_ + 1, p, a, n, b);

    encrypt_client_->async_send_request(
      request,
      [this, p, n](rclcpp::Client<rm_server::srv::ElGamalEncrypt>::SharedFuture future) {
        try {
          auto response = future.get();
          const uint64_t y1 = response->y1;
          const uint64_t y2 = response->y2;

          const uint64_t exponent = p - 1 - n;
          const uint64_t x = mod_mul(y2, mod_pow(y1, exponent, p), p);

          std_msgs::msg::Int64 result_msg;
          result_msg.data = static_cast<int64_t>(x);
          result_pub_->publish(result_msg);

          ++round_;
          waiting_service_ = false;

          RCLCPP_INFO(
            this->get_logger(),
            "[Round %d] y1=%lu y2=%lu -> x=%lu (published)",
            round_, y1, y2, x);

          if (round_ >= kTotalRounds) {
            finished_ = true;
            RCLCPP_INFO(this->get_logger(), "Task complete: %d rounds finished.", kTotalRounds);
          }
        } catch (const std::exception & e) {
          waiting_service_ = false;
          RCLCPP_ERROR(this->get_logger(), "Service call failed: %s", e.what());
        }
      });
  }

  static constexpr int kTotalRounds = 5;

  rclcpp::Subscription<rm_server::msg::GetElGamalParams>::SharedPtr params_sub_;
  rclcpp::Publisher<std_msgs::msg::Int64>::SharedPtr result_pub_;
  rclcpp::Client<rm_server::srv::ElGamalEncrypt>::SharedPtr encrypt_client_;

  std::mt19937_64 rng_;
  bool waiting_service_ {false};
  bool finished_ {false};
  int round_ {0};
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<ElGamalClient>());
  rclcpp::shutdown();
  return 0;
}
