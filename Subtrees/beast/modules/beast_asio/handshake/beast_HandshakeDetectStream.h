//------------------------------------------------------------------------------
/*
    This file is part of Beast: https://github.com/vinniefalco/Beast
    Copyright 2013, Vinnie Falco <vinnie.falco@gmail.com>

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#ifndef BEAST_HANDSHAKEDETECTSTREAM_H_INCLUDED
#define BEAST_HANDSHAKEDETECTSTREAM_H_INCLUDED

/** A stream that can detect a handshake.
*/
/** @{ */
template <class Logic>
class HandshakeDetectStream
{
protected:
    typedef boost::system::error_code error_code;

public:
    typedef Logic LogicType;

    /** Called when the state is known.
        
        This could be called from any thread, most likely an io_service
        thread but don't rely on that.

        The Callback must be allocated via operator new.
    */
    struct Callback
    {
        virtual ~Callback () { }

        /** Called for synchronous ssl detection.

            Note that the storage for the buffers passed to the
            callback is owned by the detector class and becomes
            invalid when the detector class is destroyed, which is
            a common thing to do from inside your callback.

            @param ec A modifiable error code that becomes the return
                      value of handshake.
            @param buffers The bytes that were read in.
            @param is_ssl True if the sequence is an ssl handshake.
        */
        virtual void on_detect (Logic& logic,
            error_code& ec, ConstBuffers const& buffers) = 0;

        virtual void on_async_detect (Logic& logic,
            error_code const& ec, ConstBuffers const& buffers,
                ErrorCall const& origHandler) = 0;

    #if BEAST_ASIO_HAS_BUFFEREDHANDSHAKE
        virtual void on_async_detect (Logic& logic,
            error_code const& ec, ConstBuffers const& buffers,
                TransferCall const& origHandler) = 0;
    #endif
    };
};

//------------------------------------------------------------------------------

template <typename Stream, typename Logic>
class HandshakeDetectStreamType
    : public HandshakeDetectStream <Logic>
    , public boost::asio::ssl::stream_base
    , public boost::asio::socket_base
{
private:
    typedef HandshakeDetectStreamType <Stream, Logic> this_type;
    typedef boost::asio::streambuf buffer_type;
    typedef typename boost::remove_reference <Stream>::type stream_type;

public:
    typedef typename HandshakeDetectStream <Logic> CallbackType;

    /** This takes ownership of the callback.
        The callback must be allocated with operator new.
    */
    template <typename Arg>
    HandshakeDetectStreamType (Callback* callback, Arg& arg)
        : m_callback (callback)
        , m_next_layer (arg)
        , m_stream (m_next_layer)
    {
    }

    // This puts bytes that you already have into the detector buffer
    // Any leftovers will be given to the callback.
    // A copy of the data is made.
    //
    template <typename ConstBufferSequence>
    void fill (ConstBufferSequence const& buffers)
    {
        m_buffer.commit (boost::asio::buffer_copy (
            m_buffer.prepare (boost::asio::buffer_size (buffers)),
                buffers));
    }

    // basic_io_object

    boost::asio::io_service& get_io_service ()
    {
        return m_next_layer.get_io_service ();
    }

    // basic_socket

    typedef typename stream_type::protocol_type protocol_type;
    typedef typename stream_type::lowest_layer_type lowest_layer_type;

    lowest_layer_type& lowest_layer ()
    {
        return m_next_layer.lowest_layer ();
    }

    lowest_layer_type const& lowest_layer () const
    {
        return m_next_layer.lowest_layer ();
    }

    // ssl::stream

    error_code handshake (handshake_type type, error_code& ec)
    {
        return do_handshake (type, ec, ConstBuffers ());
    }

    template <typename HandshakeHandler>
    BOOST_ASIO_INITFN_RESULT_TYPE(HandshakeHandler, void (error_code))
    async_handshake (handshake_type type, BOOST_ASIO_MOVE_ARG(HandshakeHandler) handler)
    {
#if BEAST_ASIO_HAS_FUTURE_RETURNS
        boost::asio::detail::async_result_init<
            ErrorCall, void (error_code)> init(
                BOOST_ASIO_MOVE_CAST(HandshakeHandler)(handler));
        // init.handler is copied
        m_origHandler = ErrorCall (HandshakeHandler(init.handler));
        bassert (m_origBufferedHandler.isNull ());
        async_do_handshake (type, ConstBuffers ());
        return init.result.get();
#else
        m_origHandler = ErrorCall (handler);
        bassert (m_origBufferedHandler.isNull ());
        async_do_handshake (type, ConstBuffers ());
#endif
    }

#if BEAST_ASIO_HAS_BUFFEREDHANDSHAKE
    template <typename ConstBufferSequence>
    error_code handshake (handshake_type type,
        const ConstBufferSequence& buffers, error_code& ec)
    {
        return do_handshake (type, ec, ConstBuffers (buffers));;
    }

    template <typename ConstBufferSequence, typename BufferedHandshakeHandler>
    BOOST_ASIO_INITFN_RESULT_TYPE(BufferedHandshakeHandler, void (error_code, std::size_t))
    async_handshake(handshake_type type, const ConstBufferSequence& buffers,
        BOOST_ASIO_MOVE_ARG(BufferedHandshakeHandler) handler)
    {
#if BEAST_ASIO_HAS_FUTURE_RETURNS
        boost::asio::detail::async_result_init<
            BufferedHandshakeHandler, void (error_code, std::size_t)> init(
                BOOST_ASIO_MOVE_CAST(BufferedHandshakeHandler)(handler));
        // init.handler is copied
        m_origBufferedHandler = TransferCall (BufferedHandshakeHandler(init.handler));
        bassert (m_origHandler.isNull ());
        async_do_handshake (type, ConstBuffers (buffers));
        return init.result.get();
#else
        m_origBufferedHandler = TransferCall (handler);
        bassert (m_origHandler.isNull ());
        async_do_handshake (type, ConstBuffers (buffers));
#endif
    }
#endif

    //--------------------------------------------------------------------------

    error_code do_handshake (handshake_type, error_code& ec, ConstBuffers const& buffers)
    {
        ec = error_code ();

        // Transfer caller data to our buffer.
        m_buffer.commit (boost::asio::buffer_copy (m_buffer.prepare (
            boost::asio::buffer_size (buffers)), buffers));

        do
        {
            std::size_t const available = m_buffer.size ();
            std::size_t const needed = m_logic.max_needed ();
            if (available < needed)
            {
                buffer_type::mutable_buffers_type buffers (
                    m_buffer.prepare (needed - available));
                m_buffer.commit (m_next_layer.read_some (buffers, ec));
            }

            if (! ec)
            {
                m_logic.analyze (m_buffer.data ());

                if (m_logic.finished ())
                {
                    // consume what we used (for SSL its 0)
                    std::size_t const consumed = m_logic.bytes_consumed ();
                    bassert (consumed <= m_buffer.size ());
                    m_buffer.consume (consumed);
                    m_callback->on_detect (m_logic.get (), ec,
                        ConstBuffers (m_buffer.data ()));
                    break;
                }

                // If this fails it means we will never finish
                check_postcondition (available < needed);
            }
        }
        while (! ec);

        return ec;
    }

    //--------------------------------------------------------------------------

    void async_do_handshake (handshake_type type, ConstBuffers const& buffers)
    {
        // Transfer caller data to our buffer.
        std::size_t const bytes_transferred (boost::asio::buffer_copy (
            m_buffer.prepare (boost::asio::buffer_size (buffers)), buffers));

        // bootstrap the asynchronous loop
        on_async_read_some (error_code (), bytes_transferred);
    }

    // asynchronous version of the synchronous loop found in handshake ()
    //
    void on_async_read_some (error_code const& ec, std::size_t bytes_transferred)
    {
        if (! ec)
        {
            m_buffer.commit (bytes_transferred);

            std::size_t const available = m_buffer.size ();
            std::size_t const needed = m_logic.max_needed ();

            if (bytes_transferred > 0)
                m_logic.analyze (m_buffer.data ());

            if (m_logic.finished ())
            {
                // consume what we used (for SSL its 0)
                std::size_t const consumed = m_logic.bytes_consumed ();
                bassert (consumed <= m_buffer.size ());
                m_buffer.consume (consumed);

            #if BEAST_ASIO_HAS_BUFFEREDHANDSHAKE
                if (! m_origBufferedHandler.isNull ())
                {
                    bassert (m_origHandler.isNull ());
                    // continuation?
                    m_callback->on_async_detect (m_logic.get (), ec,
                        ConstBuffers (m_buffer.data ()), m_origBufferedHandler);
                    return;
                }
            #endif

                bassert (! m_origHandler.isNull ())
                // continuation?
                m_callback->on_async_detect (m_logic.get (), ec,
                    ConstBuffers (m_buffer.data ()), m_origHandler);
                return;
            }

            // If this fails it means we will never finish
            check_postcondition (available < needed);

            buffer_type::mutable_buffers_type buffers (m_buffer.prepare (
                needed - available));

            // need a continuation hook here?
            m_next_layer.async_read_some (buffers, boost::bind (
                &this_type::on_async_read_some, this, boost::asio::placeholders::error,
                    boost::asio::placeholders::bytes_transferred));

            return;
        }

    #if BEAST_ASIO_HAS_BUFFEREDHANDSHAKE
        if (! m_origBufferedHandler.isNull ())
        {
            bassert (m_origHandler.isNull ());
            // continuation?
            m_callback->on_async_detect (m_logic.get (), ec,
                ConstBuffers (m_buffer.data ()), m_origBufferedHandler);
            return;
        }
    #endif

        bassert (! m_origHandler.isNull ())
        // continuation?
        m_callback->on_async_detect (m_logic.get (), ec,
            ConstBuffers (m_buffer.data ()), m_origHandler);
    }

private:
    ScopedPointer <typename HandshakeDetectStream <Logic>::Callback> m_callback;
    Stream m_next_layer;
    buffer_type m_buffer;
    boost::asio::buffered_read_stream <stream_type&> m_stream;
    HandshakeDetectLogicType <Logic> m_logic;
    ErrorCall m_origHandler;
    TransferCall m_origBufferedHandler;
};
/** @} */

#endif