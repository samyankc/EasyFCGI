/*

This example is taken inspiration from works on github

1. In case anyone wants to see a multi-threaded FastCGI application written with the fcgiapp library.
https://gist.github.com/dermesser/e2f9b66457ae19ebd116

2. libfcgi example
https://github.com/LuaDist/fcgi/blob/master/examples/threaded.c

Both of them used the same approach.
Setting up one single socket for all threads to share
Spawning threads, each thread performs its own accept-response loop
This approach relies on the premission for multithreaded accept calls

This example code adopts the same approach with minor modification
Also adding basic support for handling SIGINT, SIGTERM

*/

#define NO_FCGI_DEFINES
#include <fcgiapp.h>
#include <fcgios.h>
#include <concepts>
#include <string>
#include <string_view>
#include <vector>
#include <atomic>
#include <cstdlib>
#include <source_location>
#include <print>
#include <thread>
#include <sys/stat.h>
#include <sys/socket.h>
#include <csignal>

using namespace std::chrono_literals;

#define ErrorGuard( OP ) ErrorGuard_impl( OP, #OP )
auto ErrorGuard_impl( auto ErrorCode, std::string_view OperationTitle,  //
                      std::string_view CallerName = std::source_location::current().function_name() )
{
    if( ErrorCode == 0 ) return std::println( "[ OK ]  {} : {}", CallerName, OperationTitle );
    std::println( "[ Fail, Error = {} ]  {} : {}", ErrorCode, CallerName, OperationTitle );
    std::exit( ErrorCode );
}

auto TerminationSignal = std::atomic<bool>{ false };
auto TerminationHandler = []( int Signal ) {
    std::println( "\nReceiving Signal : {}", Signal );
    TerminationSignal = true;
};
auto _1 = std::signal( SIGINT, TerminationHandler );
auto _2 = std::signal( SIGTERM, TerminationHandler );

struct FileDescriptor
{
    constexpr static auto BackLogNumber = 128;  // evetually capped by /proc/sys/net/core/somaxconncat
    using ValueType = decltype( FCGX_OpenSocket( {}, {} ) );
    ValueType FD;

    constexpr FileDescriptor() : FD{ 0 } {}
    template<std::convertible_to<std::string_view> T>
    FileDescriptor( T&& SocketPath ) : FD{ FCGX_OpenSocket( std::data( std::forward<T>( SocketPath ) ), BackLogNumber ) }
    {
        chmod( std::data( SocketPath ), 0777 );  // do we need this ?
    }

    constexpr operator ValueType() const { return FD; }
};

auto WorkerTask( FileDescriptor SocketFD )
{
    FCGX_Request Request{};
    ErrorGuard( FCGX_InitRequest( &Request, SocketFD, FCGI_FAIL_ACCEPT_ON_INTR ) );

    while( FCGX_Accept_r( &Request ) == 0 )
    {
        std::println( "{} , {} Handling Request...", getpid(), std::this_thread::get_id() );
        std::this_thread::sleep_for( 400ms );
        FCGX_PutS( std::format( "{} | ", std::this_thread::get_id() ).c_str(), Request.out );

        auto Query = FCGX_GetParam( "QUERY_STRING", Request.envp );
        if( Query != nullptr )
            FCGX_PutS( Query, Request.out );
        else
            FCGX_PutS( "Cannot read FCGX_GetParam()\n", Request.out );
        FCGX_PutS( "| Testing Mesaage\n", Request.out );
        // FCGX_Finish_r( &Request );  // not required
    }

    std::println( "{} | Task Loop End", std::this_thread::get_id() );
}

int main( int argc, char** argv )
{
    const auto SocketPath = std::string_view{ "/dev/shm/delayed_response.sock" };
    const auto SocketFD = FileDescriptor{ SocketPath };

    ErrorGuard( FCGX_Init() );
    std::atexit( OS_LibShutdown );
    std::atexit( [] { std::println( "exit." ); } );

    std::thread( [SocketFD] {
        while( ! TerminationSignal ) std::this_thread::sleep_for( 500ms );
        FCGX_ShutdownPending();  // [os_unix.c:108] shutdownPending = TRUE;
        ::shutdown( SocketFD, SHUT_RDWR );
        // ::close( SocketFD );
    } ).detach();

    auto Threads = std::vector<std::jthread>{};
    Threads.reserve( 10 );
    Threads.emplace_back( WorkerTask, SocketFD );
    Threads.emplace_back( WorkerTask, SocketFD );
    Threads.emplace_back( WorkerTask, SocketFD );
    Threads.emplace_back( WorkerTask, SocketFD );
    Threads.emplace_back( WorkerTask, SocketFD );
    Threads.emplace_back( WorkerTask, SocketFD );
    Threads.emplace_back( WorkerTask, SocketFD );

    // std::ranges::for_each( Threads, &std::jthread::join );

    return 0;
}
