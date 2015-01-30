/*
 * Copyright © 2014-2015, Tolga HOŞGÖR.
 *
 * File created on: 27.01.2015
*/

/*
  This file is part of libashttp.

  libashttp is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  libashttp is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with libashttp.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "request.hpp"

#include "client.hpp"
#include "../header.hpp"

#include <boost/chrono.hpp>
#include <boost/log/trivial.hpp>

#include <array>

namespace ashttp {
namespace client {

using namespace std::placeholders;

template <class C>
Request<C>::Request(C& client, std::string resource, Millisec timeout)
    : m_client{client}
    , m_resource{std::move(resource)}
    , m_timeout{timeout}
    , m_timeoutTimer{m_client.socket().get_io_service()} {
  BOOST_LOG_TRIVIAL(trace) << "Request";
}

template <class C>
Request<C>::~Request() {
  BOOST_LOG_TRIVIAL(trace) << "~Request";
}

template <class C>
void Request<C>::timeout(Millisec timeout) {
  m_timeout = std::move(timeout);
}

template <class C>
Request<C>& Request<C>::onHeader(HeaderCallback callback) {
  m_headerCallback = std::move(callback);

  return *this;
}

template <class C>
Request<C>& Request<C>::onBodyChunk(BodyChunkCallback callback) {
  m_bodyChunkCallback = std::move(callback);

  return *this;
}

template <class C>
Request<C>& Request<C>::onTimeout(Request<C>::TimeoutCallback callback) {
  m_timeoutCallback = std::move(callback);

  return *this;
}

template <class C>
void Request<C>::cancel() {
  if (m_client.socket().lowest_layer().is_open())
    m_client.socket().lowest_layer().cancel();
}

template <class C>
void Request<C>::reset() {
  BOOST_LOG_TRIVIAL(trace) << "Request<C>::reset";

  m_headerCallback = nullptr;
  m_bodyChunkCallback = nullptr;
  m_timeoutCallback = nullptr;
}

template <class C>
void Request<C>::start() {
  BOOST_LOG_TRIVIAL(trace) << "Request<C>::fetch";

  m_client.connect(std::bind(&Request<C>::onConnect_, this, _1, _2));

  // start the timeout
  m_timedOut = false;
  m_timeoutTimer.expires_from_now(m_timeout);
  m_timeoutTimer.async_wait(std::bind(&Request<C>::onTimeout_, this, _1));
}

template <class C>
void Request<C>::headerCompleted(const Request::ErrorCode& ec, Header header) {
  BOOST_LOG_TRIVIAL(trace) << "Request<C>::callHeaderCallback ec: " << ec;

  if (m_headerCallback) {
    m_headerCallback(ec, std::move(header));
    m_headerCallback = nullptr;
  }

  if (ec) {// clear out other callbacks if there is an error (they will not be called in that case)
    m_timeoutTimer.cancel();

    m_client.requestCompleted(*this);

    m_bodyChunkCallback = nullptr;
    m_timeoutCallback = nullptr;
  }
}

template <class C>
void Request<C>::bodyChunkCompleted(const Request::ErrorCode& ec, std::istream& is, std::size_t chunkSize) {
  BOOST_LOG_TRIVIAL(trace) << "Request<C>::callBodyChunkCallback ec: " << ec;

  if (ec) {// clear out timeout callback if error occurred
    m_timeoutCallback = nullptr;

    m_timeoutTimer.cancel();
  }

  if (m_bodyChunkCallback) {
    m_bodyChunkCallback(ec, is, chunkSize);
  } else {
    m_recvBuf.consume(chunkSize + 2 /* \r\n */);
  }

  if (ec) {// if error
    m_client.requestCompleted(*this);
  } else {// no error
    if (chunkSize == 0)// it was last chunk
      m_client.requestCompleted(*this);
  }
}

template <class C>
void Request<C>::timeoutCompleted(const Request::ErrorCode& ec) {
  BOOST_LOG_TRIVIAL(trace) << "Request<C>::timeoutCompleted ec: " << ec;

  if (!ec) {// if timeout actually occurred
    if (m_timeoutCallback) {
      m_timeoutCallback();
      m_timeoutCallback = nullptr;
    }

    m_client.requestCompleted(*this);

    // clear the other callbacks
    m_headerCallback = nullptr;
    m_bodyChunkCallback = nullptr;
  }
}

template <class C>
void Request<C>::onConnect_(const Request<C>::ErrorCode& ec, const tcp::resolver::iterator& endpointIt) {
  BOOST_LOG_TRIVIAL(trace) << "Request<C>::onConnect_ ec: " << ec;

  if (!ec) {
    if (!m_timedOut) { // if we didn't already timed out before connection
      std::ostream os(&m_recvBuf);

      os << "GET " << m_resource << " HTTP/1.1\r\n"
         << "Host: " << m_client.host() << "\r\n"
                                            //         << "Connection: close\r\n"
                                            "\r\n";

      async_write(m_client.socket(), m_recvBuf, std::bind(&Request<C>::onRequestSent_, this, _1, _2));
    } else { // timed out
      m_client.socket().lowest_layer().close();
    }
  }
}

template <class C>
void Request<C>::onRequestSent_(const ErrorCode& ec, std::size_t bt) {
  BOOST_LOG_TRIVIAL(trace) << "Request<C>::onRequestSent_ ec: " << ec;

  if (!ec) {
    async_read_until(m_client.socket(), m_recvBuf, "\r\n\r\n",
                     std::bind(&Request<C>::onHeaderReceived_, this, _1, _2));
  }
}

template <class C>
void Request<C>::onHeaderReceived_(const ErrorCode& ec, std::size_t bt) {
  BOOST_LOG_TRIVIAL(trace) << "Request<C>::onHeaderReceived_ ec: " << ec;

  std::istream is{&m_recvBuf};

  try {
    if (!ec) {
      const auto headerSize = bt - 2; // do not include header end
      Header header{is, headerSize};

      // consume the extra \r\n
      m_recvBuf.consume(2);

      if (const auto transferEncoding = header.get("transfer-encoding")) { // check if chunked

        // read the chunk size
        async_read_until(m_client.socket(), m_recvBuf, "\r\n",
                         std::bind(&Request<C>::onChunkSizeReceived_, this, _1, _2));

      } else if (const auto contentLengthIt = header.get("content-length")) { // if content-length exists

        const auto contentLengthLength = std::distance(contentLengthIt->first, contentLengthIt->second);

        if (contentLengthLength <= MaxContentSize) { // check content-length header
          std::array<char, std::numeric_limits<unsigned long long>::digits10 + 1> contentLengthStr;
          std::copy(contentLengthIt->first, contentLengthIt->second, contentLengthStr.begin());
          contentLengthStr[contentLengthLength] = 0;

          const std::size_t contentLength = std::stoul(contentLengthStr.data());

          const auto bytesLeftToReceive = contentLength - std::min(contentLength, m_recvBuf.size());

          async_read(m_client.socket(), m_recvBuf.prepare(bytesLeftToReceive),
                     std::bind(&Request<C>::onBodyReceived_, this, _1, _2));

          headerCompleted(ec, std::move(header));
        } else { // too big content-length
          headerCompleted(error::fileTooLarge, std::move(header));
        }

      } else { // no content-length field in header

        // assume connection close is body end
        async_read(m_client.socket(), m_recvBuf, std::bind(&Request<C>::onBodyReceived_, this, _1, _2));

        headerCompleted(ec, std::move(header));
      }
    } else { // ec exists
      headerCompleted(ec, Header{is, 0});
    }
  } catch (const std::runtime_error& e) {
    headerCompleted(error::headerParse, Header{is, 0});
  }
}

template <class C>
void Request<C>::onBodyReceived_(const ErrorCode& ec, std::size_t bt) {
  BOOST_LOG_TRIVIAL(trace) << "Request<C>::onBodyReceived_ ec: " << ec;

  m_timeoutTimer.cancel();

  m_recvBuf.commit(bt);

  std::istream is{&m_recvBuf};

  bodyChunkCompleted(ec, is, bt);

  if (!ec)
    bodyChunkCompleted(ec, is, 0);
}

template <class C>
void Request<C>::onTimeout_(const ErrorCode& ec) {
  if (!ec) {
    m_timedOut = true;

    m_client.socket().lowest_layer().close();
  }

  timeoutCompleted(ec);
}

template <class C>
void Request<C>::onChunkSizeReceived_(const ErrorCode& ec, std::size_t bt) {
  BOOST_LOG_TRIVIAL(trace) << "Request<C>::onChunkSizeReceived_ ec: " << ec;

  // move the chunk size data to input sequence
  std::istream is{&m_recvBuf};

  // read the chunk size
  std::size_t chunkSize;
  is >> std::hex >> chunkSize;

  // skip \r\n
  is.ignore(2);

  const auto totalChunkSize = chunkSize + 2; // assume chunk size includes the \r\n

  const auto alreadyInBuffer = m_recvBuf.size();

  // maximum receive buf size is exceeded (a chunk is held at a time here)
  if (alreadyInBuffer + chunkSize <= MaxContentSize) {
    const auto chunkBytesLeftToReceive =
        totalChunkSize -
        std::min(
            totalChunkSize,
            alreadyInBuffer); /* extract the already fetched part (by async_read_until before coming here) */

    BOOST_LOG_TRIVIAL(trace) << "chunksize recv: " << alreadyInBuffer << '-' << totalChunkSize << '-'
                             << chunkBytesLeftToReceive;

    async_read(m_client.socket(), m_recvBuf.prepare(chunkBytesLeftToReceive),
               std::bind(&Request<C>::onChunkDataReceived_, this, _1, _2, chunkSize));
  } else { // file exceeds max content size
    bodyChunkCompleted(error::fileTooLarge, is, 0);
  }
}

template <class C>
void Request<C>::onChunkDataReceived_(const ErrorCode& ec, std::size_t bt,
                                      std::size_t chunkSize) {
  BOOST_LOG_TRIVIAL(trace) << "Request<C>::onChunkDataReceived_ ec: " << ec;

  // reset the noop timeout on chunk data received
  m_client.resetNoopTimeout();

  // put the received chunk to input sequence
  std::istream is{&m_recvBuf};

  // read the next chunk size
  if (chunkSize > 0) {
    m_recvBuf.commit(bt);

    bodyChunkCompleted(ec, is, chunkSize);

    is.ignore(2);

    // see if there are more chunks
    async_read_until(m_client.socket(), m_recvBuf, "\r\n",
                     std::bind(&Request<C>::onChunkSizeReceived_, this, _1, _2));
  } else { // this is the last chunk (empty chunk)
    bodyChunkCompleted(error::success, is, 0);
  }
}

}
}

template class ashttp::client::Request<ashttp::client::Client>;
template class ashttp::client::Request<ashttp::client::ClientSSL>;