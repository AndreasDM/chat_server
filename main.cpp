#include <fmt/core.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <deque>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stack>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <random>
#include <atomic>
#include <memory>
#include <thread>

#include <boost/asio.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/bind_executor.hpp>

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

typedef std::deque<std::string> message_queue;

class chat_participant {
public:
    virtual ~chat_participant() {}
    virtual void deliver(const std::string& msg) = 0;
};

class chat_room {
    // We have a set of participants
    // which is basically the websocket sessions
    std::set<std::shared_ptr<chat_participant>> participants_;

    // Limit the message queue to 100 messages
    enum { max_recent_msgs = 100 };
    message_queue recent_msgs_;

public:

    // After a new participant joins the chat room
    // we store the participant in a list.
    //
    // We also deliver all recent messages that we have (max 100)
    // to the new participant. So that they also can take part
    // of the discussion
    void join(std::shared_ptr<chat_participant> participant)
    {
        fmt::print("chat_room::join\n");

        // Store the participant
        participants_.insert(participant);

        // Deliver all recent messages to the
        // new participant
        for (const auto& msg : recent_msgs_)
            // This actually calls websocket_session::deliver
            participant->deliver(msg);
    }

    // When a participant leaves we simply erase the participant from
    // the list
    void leave(std::shared_ptr<chat_participant> participant)
    {
        fmt::print("chat_room::leave\n");
        participants_.erase(participant);
    }

    // After accepting a websocket connection and joining the room
    // and reading from the websocket a message, we end up here
    // where we want to deliver this message to the chat room
    void deliver(const std::string& msg)
    {
        // When we deliver a message we also keep track of it
        // (max 100) in the recent messages list
        recent_msgs_.push_back(msg);

        // We remove messages from the end of the queue
        // when we exceed the max message count
        while (recent_msgs_.size() > max_recent_msgs)
            recent_msgs_.pop_front();

        // Deliver message to all participants
        for (auto& participant : participants_)
            participant->deliver(msg);
    }
};

class websocket_session
: public chat_participant
, public std::enable_shared_from_this<websocket_session> {
    websocket::stream<tcp::socket> ws_;
    chat_room& room_;
    beast::flat_buffer buffer_;
    message_queue write_msgs_;
    bool writing_ = false;
    http::request<http::string_body> req_;

public:
    websocket_session(tcp::socket socket,
                      chat_room& room,
                      http::request<http::string_body> req)
        : ws_(std::move(socket))
        , room_(room)
        , req_(std::move(req))
    {}

    // When we start a web socket session we first want to
    // accept a socket connection
    // and after that we call on_accept to signal that we did so
    void start()
    {
        // Create a copy of shared pointer so that we don't
        // have lifetime problems when this function goes out of scope.
        auto self = shared_from_this();
        ws_.async_accept(req_,
            [self](beast::error_code ec) {
                self->on_accept(ec);
            });
    }

    void deliver(const std::string& msg) override
    {
        // Schedule this for execution
        boost::asio::post(
            ws_.get_executor(),
            [self = shared_from_this(), msg]()
            {
                // Put this message on the message queue
                self->write_msgs_.push_back(msg);

                // If we are not currently writing, then write the message
                if (!self->writing_)
                    self->do_write();
            });
    }

private:

    // We reach this function when we have accepted
    // a websocket connection
    void on_accept(beast::error_code ec)
    {
        if (ec) {
            std::cerr << "WebSocket async_accept error: " << ec.message() << std::endl;
            return;
        }

        // If there are no errors we join the chat room
        // Note that we create a shared pointer of this websocket session
        // And use this as a participant
        room_.join(shared_from_this());

        // We want to read from the websocket now
        do_read();
    }

    // Only after accepting a websocket connection and joining the room
    // we end up here.
    void do_read()
    {
        // Read into the buffer
        ws_.async_read(
            buffer_,
            [self = shared_from_this()](beast::error_code ec, std::size_t bytes_transferred)
            { // After reading into the buffer we call this callback
                self->on_read(ec, bytes_transferred);
            });
    }

    // After accepting a websocket connection and joining the room
    // and reading from the websocket we end up here.
    void on_read(beast::error_code ec, std::size_t /*bytes_transferred*/)
    {
        // If there were any errors we simply leave the room.
        if (ec) {
            room_.leave(shared_from_this());
            return;
        }

        // Convert the buffer data to a string
        std::string msg = beast::buffers_to_string(buffer_.data());

        // Empty the buffer
        buffer_.consume(buffer_.size());

        // The string that we received from the websocket connection
        // we want to deliver to the chat room
        room_.deliver(msg);

        // Read new messages from the websocket
        do_read();
    }

    void do_write()
    {
        writing_ = true;
        ws_.async_write(
            boost::asio::buffer(write_msgs_.front()),
            [self = shared_from_this()](beast::error_code ec, std::size_t /*bytes_transferred*/)
            {
                self->on_write(ec);
            });
    }

    // We end up here after we have sent a message over the websocket
    void on_write(beast::error_code ec)
    {
        // If error we leave the room
        if (ec) {
            room_.leave(shared_from_this());
            return;
        }

        // Remove the last message we just sent from the queue
        write_msgs_.pop_front();

        // If there are more messages to send over the websocket
        if (!write_msgs_.empty())
            // Then we send them
            do_write();
        else
            // Otherwise we say that we are done writing
            writing_ = false;
    }

};

class http_session : public std::enable_shared_from_this<http_session> {
    tcp::socket socket_;
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> req_;
    chat_room& room_;

public:
    http_session(tcp::socket socket, chat_room& room)
        : socket_(std::move(socket))
        , strand_(boost::asio::make_strand(socket_.get_executor()))
        , room_(room)
    {}

    // The first thing we do when http_session is created
    // is to `run` the session
    void run()
    {
        // Which in turn tries to read
        do_read();
    }

private:
    void do_read()
    {
        // Empty the request body
        req_ = {};

        // Attempt to read from the http session
        // Notice that we take a copy of the shared_ptr
        // To maintain lifetime even after this function goes out of scope
        http::async_read(socket_, buffer_, req_,
            boost::asio::bind_executor(strand_,
                [self = shared_from_this()](beast::error_code ec,
                                            std::size_t bytes_transferred)
                { // When the callback is called we call on_read
                    self->on_read(ec, bytes_transferred);
                }));
    }

    void on_read(beast::error_code ec, std::size_t /*bytes_transferred*/)
    {
        if (ec == http::error::end_of_stream)
            return do_close();
        if (ec)
            return;

        // Handle WebSocket upgrade
        if (websocket::is_upgrade(req_)) {
            std::make_shared<websocket_session>(std::move(socket_),
                                                room_, std::move(req_))->start();
            return;
        }

        // Handle HTTP request (serve the HTML page)
        handle_request();
    }

    void handle_request()
    {
        // This is the response body to deliver to the client
        auto res = std::make_shared<http::response<http::string_body>>();
        res->version(req_.version());
        res->result(http::status::ok);
        res->set(http::field::server, "Boost.Beast");
        res->set(http::field::content_type, "text/html");
        res->body() = get_chat_html(); // Notice the pre-defined html body
        res->prepare_payload();

        res->keep_alive(req_.keep_alive());

        // Send the http response to client
        auto self = shared_from_this();
        http::async_write(socket_, *res,
            boost::asio::bind_executor(strand_,
                [self, res](beast::error_code ec, std::size_t)
                { // When that has been done, we signal this
                    self->on_write(ec, res->need_eof());
                }));
    }

    // We have sent http response to client
    void on_write(beast::error_code ec, bool need_eof)
    {
        // Error?
        if (ec) return;

        if (need_eof) {
            beast::error_code ec_shutdown;
            socket_.shutdown(tcp::socket::shutdown_send, ec_shutdown);
            return;
        }

        // Empty request body
        req_ = {};

        // Empty buffer
        buffer_.consume(buffer_.size());

        // Now we just want to repeat
        do_read();
    }

    std::string get_chat_html()
    {
        return R"(<!DOCTYPE html>
    <html>
    <head>
        <title>Chat Room</title>
        <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
        <style>
            /* Reset margins and paddings */
            * {
                margin: 0;
                padding: 0;
                box-sizing: border-box;
            }

            body, html {
                height: 100%;
                font-family: Arial, sans-serif;
                overflow: hidden; /* Prevent body scrolling */
            }

            #container {
                display: flex;
                flex-direction: column;
                height: 100%;
            }

            #chat {
                flex: 1;
                overflow-y: auto;
                padding: 10px;
                background-color: #f9f9f9;
            }

            #input-area {
                display: flex;
                padding: 10px;
                background-color: #fff;
                border-top: 1px solid #ccc;
            }

            #message {
                flex: 1;
                padding: 10px;
                font-size: 16px;
                border: 1px solid #ccc;
                border-radius: 4px;
            }

            #send {
                padding: 10px 20px;
                font-size: 16px;
                margin-left: 10px;
                border: none;
                background-color: #28a745;
                color: #fff;
                border-radius: 4px;
            }
        </style>
    </head>
    <body>
        <div id="container">
            <div id="chat"></div>
            <div id="input-area">
                <input type="text" id="message" placeholder="Type your message..." autocomplete="off" />
                <button id="send">Send</button>
            </div>
        </div>

        <script>
            var ws;
            var chat = document.getElementById('chat');
            var messageInput = document.getElementById('message');
            var sendButton = document.getElementById('send');
            var inputArea = document.getElementById('input-area');

            function initWebSocket() {
                ws = new WebSocket('ws://localhost:12345');

                ws.onopen = function() {
                    console.log('WebSocket connection opened.');
                    sendButton.disabled = false;
                };

                ws.onmessage = function(event) {
                    var message = document.createElement('div');
                    message.textContent = event.data;
                    chat.appendChild(message);
                    chat.scrollTop = chat.scrollHeight;
                };

                ws.onclose = function() {
                    console.log('WebSocket connection closed.');
                    sendButton.disabled = true;
                };

                ws.onerror = function(error) {
                    console.error('WebSocket error:', error);
                };
            }

            sendButton.onclick = function() {
                var msg = messageInput.value.trim();
                if (msg && ws.readyState === WebSocket.OPEN) {
                    ws.send(msg);
                    messageInput.value = '';
                    // Do not focus the input field to allow the keyboard to hide
                    // messageInput.blur();
                    // Scroll to the bottom after sending
                    chat.scrollTop = chat.scrollHeight;
                }
            };

            messageInput.addEventListener('keyup', function(event) {
                if (event.key === 'Enter' || event.keyCode === 13) {
                    sendButton.click();
                }
            });

            // Variables to store the viewport height
            var vh = window.innerHeight;

            // Adjust the chat area when the keyboard appears or disappears
            function adjustChatArea() {
                window.addEventListener('resize', function() {
                    // Compare the new viewport height with the stored one
                    var newVh = window.innerHeight;
                    var isKeyboardOpen = newVh < vh;

                    if (isKeyboardOpen) {
                        // Keyboard is open
                        chat.style.height = (newVh - inputArea.offsetHeight) + 'px';
                    } else {
                        // Keyboard is closed
                        chat.style.height = '';
                        // Scroll to the bottom when keyboard closes
                        chat.scrollTop = chat.scrollHeight;
                    }
                });

                // On input focus, adjust the chat area
                messageInput.addEventListener('focus', function() {
                    setTimeout(function() {
                        var newVh = window.innerHeight;
                        chat.style.height = (newVh - inputArea.offsetHeight) + 'px';
                        chat.scrollTop = chat.scrollHeight;
                    }, 300);
                });

                // On input blur, reset the chat area
                messageInput.addEventListener('blur', function() {
                    chat.style.height = '';
                    chat.scrollTop = chat.scrollHeight;
                });
            }

            adjustChatArea();

            sendButton.disabled = true; // Disable until WebSocket is open
            initWebSocket();
        </script>
    </body>
    </html>)";
    }

    void do_close()
    {
        beast::error_code ec;
        socket_.shutdown(tcp::socket::shutdown_send, ec);
    }
};

class chat_server {
    // The chat server consists of
    // An acceptor - to accept incoming TCP connections
    tcp::acceptor acceptor_;

    // Socket - holding the connection
    tcp::socket   socket_;

    // And a chat room
    chat_room     room_;

public:

    // The chat server takes in the io_context from the main function
    // together with a port
    chat_server(boost::asio::io_context& io_context, short port)
        // Acceptor listens to selected port
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port))
        , socket_(io_context)
    {
        // Immediately when the chat server is created
        // we want to accept incoming connections
        do_accept();
    }

private:

    void do_accept()
    {
        // Accept connections into `socket_`
        acceptor_.async_accept(socket_,
            // This is the callback that will be called
            [this](boost::system::error_code ec) {
                // If no error
                if (!ec)
                    // Create `http_session` and `run()`
                    std::make_shared<http_session>(std::move(socket_), room_)->run();

                // Call this function to continue async_accept connections
                do_accept();
            });
    }

};

int main() try
{
    short port = 12345;
    boost::asio::io_context io_context;

    fmt::print("Server starting: localhost:12345\n");

    chat_server server{io_context, port};

    // Run the I/O service
    io_context.run();

} catch (std::exception& e) {
    std::cerr << "Exception: " << e.what() << "\n";
    fmt::print("Server stopping\n");
}
