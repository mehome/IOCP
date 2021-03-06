#include "Client.h"
#include "ClientMan.h"

#include "common/Log.h"
#include "common/Network.h"

#include "common/CachedAlloc.h"

#include <cassert>
#include <iostream>
#include <vector>

namespace
{

class IOEvent
{
   public:
    enum Type
    {
        CONNECT,
        RECV,
        SEND,
    };

   public:
    static IOEvent* Create(Client* clent, Type type);
    static void Destroy(IOEvent* event);

   public:
    OVERLAPPED overlapped;
    Client* client;
    Type type;

   private:
    IOEvent();
    ~IOEvent();
    IOEvent(const IOEvent&) = delete;
    IOEvent& operator=(const IOEvent&) = delete;
};

// use thread-safe memory pool
CachedAlloc eventAllocator(sizeof(IOEvent));

/* static */ IOEvent* IOEvent::Create(Client* client, Type type)
{
    IOEvent* event = static_cast<IOEvent*>(eventAllocator.get());
    ZeroMemory(event, sizeof(IOEvent));
    event->client = client;
    event->type = type;
    return event;
}

/* static */ void IOEvent::Destroy(IOEvent* event) { eventAllocator.put(event); }

void PrintConnectionInfo(SOCKET socket)
{
    std::string serverIP, clientIP;
    u_short serverPort = 0, clientPort = 0;
    Network::GetLocalAddress(socket, clientIP, clientPort);
    Network::GetRemoteAddress(socket, serverIP, serverPort);

    TRACE("Connection from ip[%s], port[%d] to ip[%s], port[%d] succeeded.", clientIP.c_str(),
          clientPort, serverIP.c_str(), serverPort);
}

}  // namespace

/* static */ void CALLBACK
Client::IoCompletionCallback(PTP_CALLBACK_INSTANCE /* Instance */, PVOID /* Context */,
                             PVOID Overlapped, ULONG IoResult, ULONG_PTR NumberOfBytesTransferred,
                             PTP_IO /* Io */)
{
    IOEvent* event = CONTAINING_RECORD(Overlapped, IOEvent, overlapped);
    assert(event);
    assert(event->client);

    if (!ClientMan::Instance()->IsAlive(event->client))
    {
        // No client for this event.
        IOEvent::Destroy(event);
        return;
    }

    if (IoResult == ERROR_SUCCESS)
    {
        switch (event->type)
        {
        case IOEvent::CONNECT:
            event->client->OnConnect();
            break;

        case IOEvent::RECV:
            if (NumberOfBytesTransferred > 0 
                && event->client->GetState() != CLOSED)
            {
                event->client->OnRecv(NumberOfBytesTransferred);
            }
            else
            {
                event->client->OnClose();
                ClientMan::Instance()->PostRemoveClient(event->client);
            }
            break;

        case IOEvent::SEND:
            event->client->OnSend(NumberOfBytesTransferred);
            break;

        default:
            assert(false);
            break;
        }
    }
    else
    {
        ERROR_CODE(IoResult, "I/O operation failed.");

        if (event->type == IOEvent::CONNECT 
            && (event->client->m_info = event->client->m_info->ai_next) != NULL)
        {
            if (!Network::ConnectEx(
                event->client->GetSocket(), 
                event->client->m_info->ai_addr, 
                event->client->m_info->ai_addrlen, 
                &event->overlapped))
            {
                int error = WSAGetLastError();

                if (error != ERROR_IO_PENDING)
                {
                    ERROR_CODE(IoResult, "I/O operation failed.");
                    ClientMan::Instance()->PostRemoveClient(event->client);
                }
                else
                {
                    StartThreadpoolIo(event->client->m_pTPIO);
                    return; // bypass Destroy()
                }
            }
            else
            {
                event->client->OnConnect();
            }
        }
        else
        {
            ClientMan::Instance()->PostRemoveClient(event->client);
        }
    }

    IOEvent::Destroy(event);
}

Client::Client() 
    : m_pTPIO(NULL), 
    m_Socket(INVALID_SOCKET), 
    m_State(WAIT),
    m_infoList(NULL),
    m_info(NULL)
{}

Client::~Client() 
{ 
    Destroy();
    if (m_infoList)
        freeaddrinfo(m_infoList);
}

bool Client::Create(short port)
{
    assert(m_Socket == INVALID_SOCKET);
    assert(m_State == WAIT);

    // Create Socket
    m_Socket = Network::CreateSocket(true, port);
    if (m_Socket == INVALID_SOCKET)
    {
        return false;
    }

    // Make the address re-usable to re-run the same client instantly.
    bool reuseAddr = true;
    if (setsockopt(m_Socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuseAddr),
                   sizeof(reuseAddr)) == SOCKET_ERROR)
    {
        ERROR_CODE(WSAGetLastError(), "setsockopt() failed with SO_REUSEADDR.");
        return false;
    }

    // Create & Start ThreaddPool for socket IO
    m_pTPIO =
        CreateThreadpoolIo(reinterpret_cast<HANDLE>(m_Socket), IoCompletionCallback, NULL, NULL);
    if (m_pTPIO == NULL)
    {
        ERROR_CODE(GetLastError(), "CreateThreadpoolIo() failed.");
        return false;
    }

    m_State = CREATED;

    return true;
}

void Client::Close()
{
    if (m_State != CLOSED)
    {
        m_State = CLOSED;
        Network::CloseSocket(m_Socket);
        CancelIoEx(reinterpret_cast<HANDLE>(m_Socket), NULL);
        m_Socket = INVALID_SOCKET;
    }
}

void Client::Destroy()
{
    Close();

    if (m_pTPIO != NULL)
    {
        WaitForThreadpoolIoCallbacks(m_pTPIO, false);
        CloseThreadpoolIo(m_pTPIO);
        m_pTPIO = NULL;
    }
}

bool Client::PostConnect(const char* ip, short port)
{
    if (m_State != CREATED)
    {
        return false;
    }

    assert(m_Socket != INVALID_SOCKET);

    // Get Address Info
    addrinfo hints;
    ZeroMemory(&hints, sizeof(addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = 0;

    char portStr[32] = "";
    if (-1 == sprintf_s(portStr, sizeof(portStr), "%d", port))
    {
        return false;
    }

    m_infoList = NULL;
    if (getaddrinfo(ip, portStr, &hints, &m_infoList) != 0)
    {
        ERROR_CODE(WSAGetLastError(), "getaddrinfo() failed.");
        return false;
    }
  
    IOEvent* event = IOEvent::Create(this, IOEvent::CONNECT);

    // loop through all the results and connect to the first we can
    m_info = m_infoList;
    for (; m_info != NULL; m_info = m_info->ai_next)
    {
        StartThreadpoolIo(m_pTPIO);

        if (!Network::ConnectEx(
            m_Socket, 
            m_info->ai_addr, 
            m_info->ai_addrlen, 
            &event->overlapped))
        {
            int error = WSAGetLastError();

            if (error != ERROR_IO_PENDING)
            {
                CancelThreadpoolIo(m_pTPIO);
                ERROR_CODE(error, "ConnectEx() failed.");
                continue;
            }
            else
            {
                return true;
            }
        }
        else
        {
            OnConnect();
            IOEvent::Destroy(event);
            return true;
        }
    }

    IOEvent::Destroy(event);
    return false;
}

void Client::PostReceive()
{
    if (m_State == CLOSED)
    {
        ClientMan::Instance()->PostRemoveClient(this);
        return;
    }

    assert(m_State == CREATED || m_State == CONNECTED);

    WSABUF recvBufferDescriptor;
    recvBufferDescriptor.buf = reinterpret_cast<char*>(m_recvBuffer);
    recvBufferDescriptor.len = Client::MAX_RECV_BUFFER;

    DWORD numberOfBytes = 0;
    DWORD recvFlags = 0;

    IOEvent* event = IOEvent::Create(this, IOEvent::RECV);

    StartThreadpoolIo(m_pTPIO);

    int error = 0;
    if (WSARecv(m_Socket, &recvBufferDescriptor, 1, &numberOfBytes, &recvFlags, &event->overlapped,
                NULL) == SOCKET_ERROR
                && (error = WSAGetLastError()) != ERROR_IO_PENDING)
    {
        CancelThreadpoolIo(m_pTPIO);

        if (m_State == CREATED)
        {
            // Even though we get successful connection event, if our first call of WSARecv
            // failed, it means we failed in connecting.
            ERROR_CODE(error, "Server cannot accept this connection.");
        }
        else
        {
            ERROR_CODE(error, "WSARecv() failed.");
        }

        // Error Handling
        ClientMan::Instance()->PostRemoveClient(this);
    }
    else
    {
        // If this is the first call of WSARecv, we can now set the state CONNECTED.
        if (m_State == CREATED)
        {
            m_State = CONNECTED;
            PrintConnectionInfo(m_Socket);
        }

        // In this case, the completion callback will have already been scheduled to be called.
    }
}

void Client::PostSend(const char* buffer, unsigned int size)
{
    if (m_State != CONNECTED)
    {
        return;
    }

    WSABUF recvBufferDescriptor;
    recvBufferDescriptor.buf = reinterpret_cast<char*>(m_sendBuffer);
    recvBufferDescriptor.len = size;

    memcpy(m_sendBuffer, buffer, size);

    DWORD numberOfBytes = size;
    DWORD sendFlags = 0;

    IOEvent* event = IOEvent::Create(this, IOEvent::SEND);

    StartThreadpoolIo(m_pTPIO);

    int ret = WSASend(m_Socket, &recvBufferDescriptor, 1, &numberOfBytes, sendFlags,
                      &event->overlapped, NULL);
    if (ret == SOCKET_ERROR)
    {
        int error = WSAGetLastError();

        if (error != ERROR_IO_PENDING)
        {
            CancelThreadpoolIo(m_pTPIO);

            ERROR_CODE(error, "WSASend() failed.");

            // Error Handling
            ClientMan::Instance()->PostRemoveClient(this);
        }
    }
    else
    {
        // In this case, the completion callback will have already been scheduled to be called.
    }
}

bool Client::Shutdown()
{
    if (m_State != CONNECTED)
    {
        return false;
    }

    assert(m_Socket != INVALID_SOCKET);

    if (shutdown(m_Socket, SD_SEND) == SOCKET_ERROR)
    {
        ERROR_CODE(WSAGetLastError(), "shutdown() failed.");
        return false;
    }

    return true;
}

void Client::OnConnect()
{
    // The socket s does not enable previously set properties or options until
    // SO_UPDATE_CONNECT_CONTEXT is set on the socket.
    if (setsockopt(m_Socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 1) == SOCKET_ERROR)
    {
        ERROR_CODE(WSAGetLastError(), "setsockopt() failed.");
    }
    else
    {
        PostReceive();
    }
}

void Client::OnRecv(DWORD dwNumberOfBytesTransfered)
{
    // Do not process packet received here.
    // Instead, publish event with the packet and call PostRecv()
    m_recvBuffer[dwNumberOfBytesTransfered] = '\0';
    TRACE("OnRecv() : %s", m_recvBuffer);

    // To maximize performance, post recv request ASAP.
    PostReceive();
}

void Client::OnSend(DWORD dwNumberOfBytesTransfered)
{
    TRACE("OnSend() : %d", dwNumberOfBytesTransfered);
}

void Client::OnClose()
{
    TRACE("OnClose()");

    // Destroy();
}
