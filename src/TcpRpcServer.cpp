/**
 *          Copyright Springbeats Sarl 2013.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file ../LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
#include "TcpRpcServer.hpp"
#include "ThreadSafeMap.hpp"
#include <boost/bind.hpp>
#include <sstream>

using std::ostringstream;

namespace pbrpcpp {
    TcpRpcServer::ClientData::ClientData( int clientId, boost::asio::io_service& io_service )
    : clientId_(clientId),
    clientSock_(io_service)
    {

    }

    TcpRpcServer::ClientData::~ClientData() {
        boost::system::error_code ec;
        GOOGLE_LOG( INFO ) << "TcpRpcServer::ClientData::~ClientData";
        clientSock_.cancel( ec );
        clientSock_.shutdown( tcp::socket::shutdown_both, ec );
        clientSock_.close( ec );
    }

    bool TcpRpcServer::ClientData::extractMessage(string& msg) {
        try {
            return RpcMessage::extractNetPacket( receivedMsg_, msg );                        
        } catch (...) {
            boost::system::error_code ec;

            clientSock_.close(ec);
        }
        return false;
    }
            
    TcpRpcServer::TcpRpcServer( const string& listenAddr, const string& listenPort)
    :listenAddr_( listenAddr ),
    listenPort_( listenPort ),
    nextClientId_( 0 ),
    io_service_stopped_( true ),
    clientDataMgr_( new ThreadSafeMap< int, shared_ptr<ClientData> >() )
    {        
    }
    
    TcpRpcServer::~TcpRpcServer() {
        Shutdown();
    }
    
    void TcpRpcServer::Run() {
        boost::system::error_code ec;

        tcp::resolver resolver(io_service_ );
        tcp::resolver::query query( listenAddr_, listenPort_ );
        tcp::resolver::iterator iter = resolver.resolve(query, ec );
        
        if( ec ) {
            GOOGLE_LOG( FATAL ) << "fail to resolve listening address " << listenAddr_ << ":" << listenPort_;
        } else {
            GOOGLE_LOG( INFO ) << "start to accept TCP connection";

            acceptor_.reset( new tcp::acceptor( io_service_, *iter ));
            acceptor_->set_option( tcp::socket::reuse_address(true), ec );
            startAccept();

            io_service_stopped_ = false;
            io_service_.run( ec );            
            io_service_stopped_ = true;
        }
    }
    
    void TcpRpcServer::Shutdown() {        
        if( io_service_stopped_ ) {
            return;
        }
        
        if( acceptor_ ) {
            boost::system::error_code ec;
            acceptor_->cancel( ec );
            acceptor_->close( ec );
        }
        
        while( getProcessingRequests() > 0 ) {
            boost::this_thread::yield();
        }
        
        clientDataMgr_->erase_all();

        io_service_.stop();
        
        while( !io_service_stopped_ ) {
            boost::this_thread::yield();
        }
    }

    bool TcpRpcServer::getLocalEndpoint( tcp::endpoint& ep ) const {
            shared_ptr<tcp::acceptor> tmp = acceptor_;
            if( tmp ) {
                boost::system::error_code ec;                
                ep = tmp->local_endpoint( ec );
                return !ec;
            } else {
                return false;
            }
        }
    
    bool TcpRpcServer::getLocalEndpoint( string& addr, string& port ) const {
        tcp::endpoint ep;
        
        if( getLocalEndpoint( ep )) {
            addr = ep.address().to_string();
            ostringstream out;
            
            out << ep.port();
            port = out.str();
            return true;
        }
        return false;
    }
    
    void TcpRpcServer::sendResponse( int clientId, const string& msg ) {
        shared_ptr< ClientData > clientData = clientDataMgr_->get( clientId );
        if( clientData ) {
            
            shared_ptr<string> s( new string( RpcMessage::serializeNetPacket( msg )) );

            GOOGLE_LOG( INFO ) << "send response to client with " << s->length() << " bytes";
            
            boost::asio::async_write( clientData->clientSock_,
                    boost::asio::buffer( s->data(), s->length() ),
                    boost::asio::transfer_all(),
                    boost::bind( &TcpRpcServer::messageSent, this, _1, _2, s ) );
        } else {
            GOOGLE_LOG( ERROR ) << "fail to send response because the client is already disconnected";
        }
    }
    
    void TcpRpcServer::startAccept() {
        shared_ptr<ClientData> clientData( new ClientData( nextClientId_++, io_service_ ) );
        acceptor_->async_accept( clientData->clientSock_, boost::bind( &TcpRpcServer::connAccepted, this, clientData, _1 ));
    }
    
    void TcpRpcServer::connAccepted( shared_ptr<ClientData> clientData, const boost::system::error_code& ec ) {
        if( ec || stop_ ) {
            GOOGLE_LOG( ERROR ) << "fail to accept connection from client";
        } else {
            GOOGLE_LOG( INFO ) << "a client connection is accepted";
            boost::system::error_code error;
            clientData->clientSock_.set_option(tcp::socket::reuse_address(true), error );
            clientDataMgr_->insert( clientData->clientId_, clientData );
            startRead( clientData );
            startAccept();
        }
    }
    
    void TcpRpcServer::startRead( shared_ptr<ClientData> clientData ) {
        if( clientData ) {
            boost::asio::async_read( clientData->clientSock_, 
                    boost::asio::buffer( clientData->msgBuffer_, sizeof( clientData->msgBuffer_ ) ),
                    boost::asio::transfer_at_least(1),
                    boost::bind( &TcpRpcServer::clientDataReceived, this, _1, _2, clientData ) );
        }
    }
    
    void TcpRpcServer::clientDataReceived( const boost::system::error_code& ec, 
                        std::size_t bytes_transferred, 
                        shared_ptr<ClientData> clientData ) {
        if( ec ) {
            GOOGLE_LOG( ERROR ) << "fail to receive data from client";
            clientDataMgr_->erase( clientData->clientId_ );
            return;
        }
        
        try {
            GOOGLE_LOG( INFO ) << bytes_transferred << " bytes received from client";
            clientData->receivedMsg_.append( clientData->msgBuffer_, bytes_transferred );
            string msg;
            while( clientData->extractMessage( msg ) ) {
                GOOGLE_LOG(INFO) << "a message is received";
                messageReceived( clientData->clientId_, msg );
            }
            
            startRead( clientData );
        }catch( ... ) {
        }                
    }
    
    
    void TcpRpcServer::messageSent( const boost::system::error_code& ec, 
                        std::size_t bytes_transferred, 
                        shared_ptr<string> buf ) {
        if( ec ) {
            GOOGLE_LOG( ERROR ) << "fail to send message to client";
        } else {
            GOOGLE_LOG( INFO ) << "success to send " << bytes_transferred << " bytes message to client";
        }
    }
    
    
}//end name space pbrpcpp
