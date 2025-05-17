#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/strand.hpp>
#include <boost/json/src.hpp>
#include <iostream>
#include <string>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <thread>
#include <atomic>
#include <iomanip>
#include <sstream>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

class CoinGeckoPriceFetcher {
    struct CoinData {
        std::string symbol;
        boost::json::value price;
        boost::json::value market_cap;
    };

    std::vector<std::pair<std::string, CoinData>> coins_data_;
    mutable std::mutex data_mutex_;
    std::atomic<bool> running_{ false };
    net::io_context io_context_;
    std::thread worker_thread_;

public:
    CoinGeckoPriceFetcher() : running_(true) {
        worker_thread_ = std::thread([this]() {
            while (running_) {
                try {
                    fetch_data();
                    std::this_thread::sleep_for(std::chrono::minutes(5));
                }
                catch (const std::exception& e) {
                    std::cerr << "Error: " << e.what() << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(30));
                }
            }
            });
    }

    ~CoinGeckoPriceFetcher() {
        running_ = false;
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }

    std::vector<CoinData> get_coins_data() const {
        std::lock_guard<std::mutex> lock(data_mutex_);
        std::vector<CoinData> result;
        for (const auto& item : coins_data_) {
            result.push_back(item.second);
        }
        return result;
    }

private:
    void fetch_data() {
        try {
            // 1. Подготовка SSL контекста
            ssl::context ctx(ssl::context::tlsv12_client);
            ctx.set_default_verify_paths();

            // 2. Создание потока
            tcp::resolver resolver(io_context_);
            beast::ssl_stream<beast::tcp_stream> stream(io_context_, ctx);

            // 3. Установка SNI
            if (!SSL_set_tlsext_host_name(stream.native_handle(), "api.coingecko.com")) {
                throw beast::system_error(
                    beast::error_code(
                        static_cast<int>(::ERR_get_error()),
                        net::error::get_ssl_category()));
            }

            // 4. Разрешение имени хоста и подключение
            auto const results = resolver.resolve("api.coingecko.com", "443");
            beast::get_lowest_layer(stream).connect(results);
            stream.handshake(ssl::stream_base::client);

            std::vector<std::pair<std::string, CoinData>> new_data;
            const int total_coins_needed = 1000;
            const int per_page = 250;
            int pages_to_fetch = (total_coins_needed + per_page - 1) / per_page;
            new_data.reserve(total_coins_needed);

            for (int page = 1; page <= pages_to_fetch; ++page) {
                // 5. Формируем HTTP-GET запрос с пагинацией
                std::string target = "/api/v3/coins/markets?vs_currency=usd"
                    "&order=market_cap_desc"
                    "&per_page=" + std::to_string(per_page) +
                    "&page=" + std::to_string(page) +
                    "&sparkline=false"
                    "&price_change_percentage=24h";

                http::request<http::string_body> req{ http::verb::get, target, 11 };
                req.set(http::field::host, "api.coingecko.com");
                req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

                // 6. Отправляем запрос и получаем ответ
                http::write(stream, req);
                beast::flat_buffer buffer;
                http::response<http::string_body> res;
                http::read(stream, buffer, res);

                // 7. Разбираем JSON
                auto json_data = boost::json::parse(res.body()).as_array();

                //std::cout << "Raw response: " << res;

                for (const auto& item : json_data) {
                    try {
                        const auto& obj = item.as_object();
                        CoinData coin;

                        coin.symbol = obj.at("symbol").as_string().c_str();

                        if (auto* price_ptr = obj.if_contains("current_price")) {
                            if (price_ptr->is_number() || price_ptr->is_null()) {
                                coin.price = *price_ptr;
                            }

                            if (auto* cap_ptr = obj.if_contains("market_cap")) {
                                if (cap_ptr->is_number() || cap_ptr->is_null()) {
                                    coin.market_cap = *cap_ptr;
                                }
                            }

                            new_data.emplace_back(coin.symbol, std::move(coin));

                            if (new_data.size() >= total_coins_needed) break;
                        }
                    }
                    catch (const std::exception& e) {
                        std::cerr << "Error processing coin: " << e.what() << std::endl;
                    }
                }

                if (new_data.size() >= total_coins_needed) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            {
                std::lock_guard<std::mutex> lock(data_mutex_);
                coins_data_ = std::move(new_data);
            }

            std::cout << "Successfully updated " << coins_data_.size() << " coins" << std::endl;

            beast::error_code ec;
            stream.shutdown(ec);
        }
        catch (const std::exception& e) {
            std::cerr << "Fetch error: " << e.what() << std::endl;
            throw;
        }
    }
};

std::unique_ptr<CoinGeckoPriceFetcher> price_fetcher;

std::string format_number(const boost::json::value& num) {
    if (num.is_number()) {
        if (num.is_double()) {
            double value = num.as_double();
            std::ostringstream oss;
            oss << std::fixed;

            if (value == std::floor(value)) {
                oss << std::setprecision(0);
            }
            else if (value < 1.0) {
                oss << std::setprecision(6);
            }
            else {
                oss << std::setprecision(2);
            }

            oss << value;
            std::string s = oss.str();

            s.erase(s.find_last_not_of('0') + 1, std::string::npos);
            if (s.back() == '.') s.pop_back();
            return s;
        }
        return std::to_string(num.as_int64());
    }
    return "null";
}

http::response<http::string_body> handle_request(http::request<http::string_body> const& req) {
    if (req.method() == http::verb::get && req.target() == "/api/prices") {
        auto coins_data = price_fetcher->get_coins_data();

        std::string json_body = "[";
        bool first = true;

        for (const auto& coin : coins_data) {
            if (!first) {
                json_body += ",";
            }
            first = false;

            json_body += "{";
            json_body += "\"symbol\":\"" + coin.symbol + "\",";
            json_body += "\"price\":" + format_number(coin.price) + ",";
            json_body += "\"market_cap\":" + format_number(coin.market_cap);
            json_body += "}";
        }

        json_body += "]";

        http::response<http::string_body> res{ http::status::ok, req.version() };
        res.set(http::field::content_type, "application/json");
        res.body() = json_body;
        res.prepare_payload();
        return res;
    }

    http::response<http::string_body> res{ http::status::not_found, req.version() };
    res.set(http::field::content_type, "text/plain");
    res.body() = "Not Found";
    res.prepare_payload();
    return res;
}

class Session : public std::enable_shared_from_this<Session> {
    tcp::socket socket_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> req_;

public:
    explicit Session(tcp::socket socket) : socket_(std::move(socket)) {}

    void run() {
        do_read();
    }

private:
    void do_read() {
        auto self(shared_from_this());
        http::async_read(
            socket_,
            buffer_,
            req_,
            [this, self](beast::error_code ec, std::size_t bytes_transferred) {
                if (ec == http::error::end_of_stream) {
                    socket_.shutdown(tcp::socket::shutdown_send, ec);
                    return;
                }
                if (ec) {
                    std::cerr << "Read error: " << ec.message() << std::endl;
                    return;
                }
                do_write(handle_request(req_));
            });
    }

    void do_write(http::response<http::string_body> res) {
        auto self(shared_from_this());
        auto sp = std::make_shared<http::response<http::string_body>>(std::move(res));
        http::async_write(
            socket_,
            *sp,
            [this, self, sp](beast::error_code ec, std::size_t) {
                if (ec) {
                    std::cerr << "Write error: " << ec.message() << std::endl;
                    return;
                }
                if (sp->keep_alive()) {
                    do_read();
                }
                else {
                    beast::error_code shutdown_ec;
                    socket_.shutdown(tcp::socket::shutdown_send, shutdown_ec);
                }
            });
    }
};

class Listener : public std::enable_shared_from_this<Listener> {
    net::io_context& ioc_;
    tcp::acceptor acceptor_;

public:
    Listener(net::io_context& ioc, tcp::endpoint endpoint)
        : ioc_(ioc), acceptor_(ioc) {
        beast::error_code ec;

        acceptor_.open(endpoint.protocol(), ec);
        if (ec) {
            std::cerr << "Open error: " << ec.message() << std::endl;
            return;
        }

        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec) {
            std::cerr << "Set option error: " << ec.message() << std::endl;
            return;
        }

        acceptor_.bind(endpoint, ec);
        if (ec) {
            std::cerr << "Bind error: " << ec.message() << std::endl;
            return;
        }

        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec) {
            std::cerr << "Listen error: " << ec.message() << std::endl;
            return;
        }
    }

    void run() {
        do_accept();
    }

private:
    void do_accept() {
        acceptor_.async_accept(
            net::make_strand(ioc_.get_executor()),
            [self = shared_from_this()](beast::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::make_shared<Session>(std::move(socket))->run();
                }
                self->do_accept();
            });
    }
};

int main() {
    try {
        price_fetcher = std::make_unique<CoinGeckoPriceFetcher>();

        auto const address = net::ip::make_address("0.0.0.0");
        unsigned short port = 8080;

        net::io_context ioc{ 1 };

        auto listener = std::make_shared<Listener>(ioc, tcp::endpoint{ address, port });
        listener->run();

        ioc.run();
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}