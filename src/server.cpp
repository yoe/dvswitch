// Copyright 2007 Ben Hutchings and Tore Sinding Bekkedal.
// See the file "COPYING" for licence details.

#include <cstring>
#include <functional>
#include <iostream>
#include <ostream>
#include <stdexcept>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <boost/bind.hpp>
#include <boost/variant.hpp>

#include <libdv/dv.h>

#include "frame.h"
#include "mixer.hpp"
#include "os_error.hpp"
#include "protocol.h"
#include "server.hpp"
#include "socket.h"

class server::connection
{
public:
    enum send_status {
	send_failed,
	sent_some,
	sent_all
    };

    virtual ~connection() {}
    connection * do_receive();
    virtual send_status do_send() { return send_failed; }

protected:
    struct receive_buffer
    {
	receive_buffer()
	    : pointer(0),
	      size(0)
	{}
	receive_buffer(uint8_t * pointer, std::size_t size)
	    : pointer(pointer),
	      size(size)
	{}
	uint8_t * pointer;
	std::size_t size;
    };

    connection(server & server, auto_fd socket);

    server & server_;
    auto_fd socket_;

private:
    virtual receive_buffer get_receive_buffer() = 0;
    virtual connection * handle_complete_receive() = 0;
    virtual std::ostream & print_identity(std::ostream &) = 0;

    receive_buffer receive_buffer_;
};

class server::unknown_connection : public connection
{
public:
    unknown_connection(server & server, auto_fd socket);

private:
    virtual receive_buffer get_receive_buffer();
    virtual connection * handle_complete_receive();
    virtual std::ostream & print_identity(std::ostream &);

    receive_buffer identify_client_type();

    uint8_t greeting_[4];
};

class server::source_connection : public connection
{
public:
    source_connection(server & server, auto_fd socket);
    virtual ~source_connection();

private:
    virtual receive_buffer get_receive_buffer();
    virtual connection * handle_complete_receive();
    virtual std::ostream & print_identity(std::ostream &);

    dv_decoder_t * decoder_;

    mixer::frame_ptr frame_;
    bool first_sequence_;

    mixer::source_id source_id_;
};

class server::sink_connection : public connection, private mixer::sink
{
public:
    sink_connection(server &, auto_fd socket, bool is_raw);
    virtual ~sink_connection();

private:
    virtual send_status do_send();
    virtual receive_buffer get_receive_buffer();
    virtual connection * handle_complete_receive();
    virtual std::ostream & print_identity(std::ostream &);

    virtual void put_frame(const mixer::frame_ptr & frame);

    receive_buffer handle_unexpected_input();

    bool is_raw_;
    mixer::sink_id sink_id_;
    std::size_t frame_pos_;

    boost::mutex mutex_; // controls access to the following
    ring_buffer<mixer::frame_ptr, 30> frames_;
    bool overflowed_;
};

server::server(const std::string & host, const std::string & port,
	       mixer & mixer)
    : mixer_(mixer),
      listen_socket_(create_listening_socket(host.c_str(), port.c_str())),
      message_pipe_(O_NONBLOCK, O_NONBLOCK)
{
    server_thread_.reset(new boost::thread(boost::bind(&server::serve, this)));
}

server::~server()
{
    int quit_message = -1;
    write(message_pipe_.writer.get(), &quit_message, sizeof(int));
    server_thread_->join();
}

void server::serve()
{
    std::vector<pollfd> poll_fds(2);
    std::vector<connection *> connections;
    poll_fds[0].fd = message_pipe_.reader.get();
    poll_fds[0].events = POLLIN;
    poll_fds[1].fd = listen_socket_.get();
    poll_fds[1].events = POLLIN;

    for (;;)
    {
	int count = poll(&poll_fds[0], poll_fds.size(), -1);
	if (count < 0)
	{
	    int error = errno;
	    if (error == EAGAIN || error == EINTR)
		continue;
	    std::cerr << "ERROR: poll: " << std::strerror(errno) << "\n";
	    break;
	}

	// Check message pipe
	if (poll_fds[0].revents & POLLIN)
	{
	    int messages[1024];
	    ssize_t size = read(message_pipe_.reader.get(),
				messages, sizeof(messages));
	    if (size > 0)
	    {
		for (std::size_t i = 0;
		     (i + 1) * sizeof(int) <= std::size_t(size);
		     ++i)
		{
		    // Each message is either -1 (quit) or the number of an
		    // FD that we now want to write to.
		    if (messages[i] == -1)
			goto exit;

		    for (std::size_t j = 2; j != poll_fds.size(); ++j)
		    {
			if (poll_fds[j].fd == messages[i])
			{
			    poll_fds[j].events |= POLLOUT;
			    break;
			}
		    }
		}
	    }
	}

	// Check listening socket
	if (poll_fds[1].revents & POLLIN)
	{
	    auto_fd conn_socket(accept(listen_socket_.get(), 0, 0));
	    if (conn_socket.get() >= 0)
	    {
		os_check_nonneg("fcntl",
				fcntl(conn_socket.get(), F_SETFL, O_NONBLOCK));
		pollfd new_poll_fd = { conn_socket.get(), POLLIN, 0 };
		connections.push_back(
		    new unknown_connection(*this, conn_socket));
		poll_fds.push_back(new_poll_fd);
	    }
	}

	// Check client connections
	for (std::size_t i = 0; i != connections.size();)
	{
	    short revents = poll_fds[2 + i].revents;
	    bool should_drop = false;
	    try
	    {
		if (revents & (POLLHUP | POLLERR))
		{
		    should_drop = true;
		}
		else if (revents & POLLIN)
		{
		    connection * new_connection =
			connections[i]->do_receive();
		    if (!new_connection)
		    {
			should_drop = true;
		    }
		    else if (new_connection != connections[i])
		    {
			delete connections[i];
			connections[i] = new_connection;
		    }
		}
		else if (revents & POLLOUT)
		{
		    switch (connections[i]->do_send())
		    {
		    case connection::send_failed:
			should_drop = true;
			break;
		    case connection::sent_some:
			break;
		    case connection::sent_all:
			poll_fds[2 + i].events &= ~POLLOUT;
		        break;
		    }
		}
	    }
	    catch (std::exception & e)
	    {
		std::cerr << "ERROR: " << e.what() << "\n";
		should_drop = true;
	    }

	    if (should_drop)
	    {
		delete connections[i];
		connections.erase(connections.begin() + i);
		poll_fds.erase(poll_fds.begin() + 2 + i);
	    }
	    else
	    {
		++i;
	    }
	}
    }

exit:
    std::vector<connection *>::iterator
	it = connections.begin(), end = connections.end();
    while (it != end)
	delete *it++;
}

void server::enable_output_polling(int fd)
{
    os_check_zero("write",
		  write(message_pipe_.writer.get(), &fd, sizeof(int))
		  - sizeof(int));
}

server::connection::connection(server & server, auto_fd socket)
    : server_(server),
      socket_(socket)
{}

server::connection * server::connection::do_receive()
{
    connection * result = 0;

    if (receive_buffer_.size == 0)
    {
	receive_buffer_ = get_receive_buffer();
	assert(receive_buffer_.pointer && receive_buffer_.size);
    }

    ssize_t received_size = read(socket_.get(),
				 receive_buffer_.pointer,
				 receive_buffer_.size);
    if (received_size > 0)
    {
	receive_buffer_.pointer += received_size;
	receive_buffer_.size -= received_size;
	if (receive_buffer_.size == 0)
	    result = handle_complete_receive();
	else
	    result = this;
    }
    else if (received_size == -1 && errno == EWOULDBLOCK)
    {
	// This is expected when the socket buffer is empty
	result = this;
    }

    if (!result)
    {
	// XXX We should distinguish several kinds of failure: network
	// problems, normal disconnection, protocol violation, and
	// resource allocation failure.
	std::cerr << "WARN: Dropping connection from ";
	print_identity(std::cerr) << "\n";
    }

    return result;
}

server::sink_connection::sink_connection(server & server, auto_fd socket,
					 bool is_raw)
    : connection(server, socket),
      is_raw_(is_raw),
      frame_pos_(0),
      overflowed_(false)
{
    sink_id_ = server_.mixer_.add_sink(this);
}

server::sink_connection::~sink_connection()
{	  
    server_.mixer_.remove_sink(sink_id_);
}

server::connection::send_status server::sink_connection::do_send()
{
    send_status result = send_failed;
    bool finished_frame = false;

    do
    {
	mixer::frame_ptr frame;
	{
	    boost::mutex::scoped_lock lock(mutex_);
	    if (overflowed_)
		break;
	    if (finished_frame)
	    {
		frames_.pop();
		finished_frame = false;
	    }
	    if (frames_.empty())
	    {
		result = sent_all;
		break;
	    }
	    frame = frames_.front();
	}

	uint8_t frame_header[SINK_FRAME_HEADER_SIZE] = {};
	if (!is_raw_)
	{
	    frame_header[SINK_FRAME_CUT_FLAG_POS] =
		frame->cut_before ? 'C' : 0;
	    // rest of header left as zero for expansion
	}
	iovec io_vector[2] = {
	    { frame_header,  SINK_FRAME_HEADER_SIZE },
	    { frame->buffer, frame->size }
	};
	int done_count = is_raw_ ? 1 : 0;
	std::size_t rel_pos = frame_pos_;
	while (rel_pos >= io_vector[done_count].iov_len)
	{
	    rel_pos -= io_vector[done_count].iov_len;
	    ++done_count;
	}
	io_vector[done_count].iov_base =
	    static_cast<char *>(io_vector[done_count].iov_base) + rel_pos;
	io_vector[done_count].iov_len -= rel_pos;
	  
	ssize_t sent_size = writev(socket_.get(),
				   io_vector + done_count,
				   2 - done_count);
	if (sent_size > 0)
	{
	    frame_pos_ += sent_size;
	    if (frame_pos_
		== (is_raw_ ? 0 : SINK_FRAME_HEADER_SIZE) + frame->size)
	    {
		finished_frame = true;
		frame_pos_ = 0;
	    }
	    result = sent_some;
	}
	else if (sent_size == -1 && errno == EWOULDBLOCK)
	{
	    result = sent_some;
	}
    }
    while (finished_frame);

    if (result == send_failed)
    {
	// XXX We should distinguish several kinds of failure: network
	// problems, normal disconnection, protocol violation, and
	// resource allocation failure.
	std::cerr << "WARN: Dropping connection from sink "
		  << 1 + sink_id_ << "\n";
    }

    return result;
}

server::unknown_connection::unknown_connection(server & server, auto_fd socket)
    : connection(server, socket)
{}

server::connection::receive_buffer
server::unknown_connection::get_receive_buffer()
{
    return receive_buffer(greeting_, sizeof(greeting_));
}

server::connection * server::unknown_connection::handle_complete_receive()
{
    enum {
	client_type_unknown,
	client_type_source,     // source which sends greeting (>= 0.3)
	client_type_sink,       // sink which wants DIF with control headers
	client_type_raw_sink,   // sink which wants raw DIF
    } client_type;

    if (std::memcmp(greeting_, GREETING_SOURCE, GREETING_SIZE) == 0)
	client_type = client_type_source;
    else if (std::memcmp(greeting_, GREETING_SINK, GREETING_SIZE)
	     == 0)
	client_type = client_type_sink;
    else if (std::memcmp(greeting_, GREETING_RAW_SINK, GREETING_SIZE)
	     == 0)
	client_type = client_type_raw_sink;
    else
	client_type = client_type_unknown;

    switch (client_type)
    {
    case client_type_source:
	return new source_connection(server_, socket_);
    case client_type_sink:
    case client_type_raw_sink:
	return new sink_connection(server_, socket_,
				   client_type == client_type_raw_sink);
    default:
	return 0;
    }
}

std::ostream & server::unknown_connection::print_identity(std::ostream & os)
{
    return os << "unknown client";
}

server::source_connection::source_connection(server & server, auto_fd socket)
    : connection(server, socket),
      decoder_(dv_decoder_new(0, true, true)),
      frame_(server_.mixer_.allocate_frame()),
      first_sequence_(true)
{
    if (!decoder_)
	throw std::bad_alloc();
    source_id_ = server_.mixer_.add_source();
}

server::source_connection::~source_connection()
{
    server_.mixer_.remove_source(source_id_);
    dv_decoder_free(decoder_);
}

server::connection::receive_buffer
server::source_connection::get_receive_buffer()
{
    if (first_sequence_)
	return receive_buffer(frame_->buffer, DIF_SEQUENCE_SIZE);
    else
	return receive_buffer(frame_->buffer + DIF_SEQUENCE_SIZE,
			      frame_->size - DIF_SEQUENCE_SIZE);
}

server::connection * server::source_connection::handle_complete_receive()
{
    if (first_sequence_)
    {
	if (dv_parse_header(decoder_, frame_->buffer) >= 0)
	{
	    frame_->system = decoder_->system;
	    frame_->size = decoder_->frame_size;
	    first_sequence_ = false;
	    return this;
	}
    }
    else // !first_sequence_
    {
	server_.mixer_.put_frame(source_id_, frame_);
	frame_.reset();
	frame_ = server_.mixer_.allocate_frame();
	first_sequence_ = true;
	return this;
    }

    return 0;
}

std::ostream & server::source_connection::print_identity(std::ostream & os)
{
    return os << "source " << 1 + source_id_;
}

server::connection::receive_buffer
server::sink_connection::get_receive_buffer()
{
    static uint8_t dummy;
    return receive_buffer(&dummy, sizeof(dummy));
}

server::connection * server::sink_connection::handle_complete_receive()
{
    return 0;
}

std::ostream & server::sink_connection::print_identity(std::ostream & os)
{
    return os << "sink " << 1 + sink_id_;
}

void server::sink_connection::put_frame(const mixer::frame_ptr & frame)
{
    bool was_empty = false;
    {
	boost::mutex::scoped_lock lock(mutex_);
	if (frames_.full())
	{
	    overflowed_ = true;
	}
	else
	{
	    if (frames_.empty())
		was_empty = true;
	    frames_.push(frame);
	}
    }
    if (was_empty)
	server_.enable_output_polling(socket_.get());
}
