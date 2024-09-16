#ifndef _EASY_FCGI_HPP
#define _EASY_FCGI_HPP
#include <iterator>
#define NO_FCGI_DEFINES
#include <memory>
#include <fcgiapp.h>
#include <fcgios.h>
#include <concepts>
#include <vector>
#include <atomic>
#include <cstdlib>
#include <source_location>
#include <filesystem>
#include <print>
#include <thread>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <csignal>
#include "EasyString.h"
// #include <chrono>
using namespace std::chrono_literals;

// ErrorGuard( Function Call / ErrorCode )
// check ErrorCode :   0    =>  program proceeds
//                   other  =>  invoke std::exit(ErrorCode)
#define ErrorGuard( OP ) ErrorGuard_impl( OP, #OP )
auto ErrorGuard_impl( std::integral auto ErrorCode, std::string_view OperationTitle,  //
                      std::string_view CallerName = std::source_location::current().function_name() )
{
    if( ErrorCode == 0 ) return std::println( "[ OK ]  {} : {}", CallerName, OperationTitle );
    std::println( "[ Fail, Error = {} ]  {} : {}", ErrorCode, CallerName, OperationTitle );
    std::exit( ErrorCode );
}

namespace EasyFCGI
{
    namespace std_fs = std::filesystem;
    struct Config
    {
        // evetually capped by /proc/sys/net/core/somaxconncat,eg.4096
        constexpr static auto DefaultBackLogNumber = 128;

        // static auto SocketDirectoryPrefix = std::string_view{"/tmp"};
        constexpr static auto SocketDirectoryPrefix = std::string_view{ "/dev/shm/" };
        constexpr static auto SocketExtensionSuffix = std::string_view{ ".sock" };

        constexpr static auto CommandLineBufferMax = 4096uz;
        inline static auto CommandLineBuffer = std::vector<char>{};
        inline static auto CommandLine = [] {
            auto Buffer = std::array<char, CommandLineBufferMax>{ '\0' };
            auto fin = std::fopen( "/proc/self/cmdline", "rb" );
            auto Loaded = std::fread( Buffer.data(), sizeof( Buffer[0] ), Buffer.size(), fin );
            std::fclose( fin );
            if( Loaded )
            {
                CommandLineBuffer = std::vector( Buffer.begin(), std::next( Buffer.begin(), Loaded ) );
                CommandLineBuffer.resize( Loaded - 1 );  // bury the last \0
            }

            return std::views::split( CommandLineBuffer, '\0' )  //
                   | std::views::transform( std::ranges::data )  //
                   | std::ranges::to<std::vector>();
        }();

        inline static auto DefaultSocketPath = std_fs::path( Config::SocketDirectoryPrefix ) /  //
                                               std_fs::path( CommandLine[0] )
                                                   .filename()                                  //
                                                   .replace_extension( Config::SocketExtensionSuffix );

        inline static auto OptionSocketPath = [] -> std::optional<std_fs::path> {
            // for( int i = 1; i < CommandLine.size() - 1; ++i )
            //     if( auto LaunchOption = std::string_view{ CommandLine[i] };  //
            //         LaunchOption == "-s" || LaunchOption == "--socket" )
            //         return std_fs::canonical( CommandLine[i + 1] );
            for( auto&& [Option, OptionArg] : CommandLine | std::views::adjacent<2> )
                if( Option == std::string_view{ "-s" } )  //
                    return OptionArg;
            return {};
        }();
    };

    static auto TerminationSignal = std::atomic<bool>{ false };
    static auto TerminationHandler = []( int Signal ) {
        std::println( "\nReceiving Signal : {}", Signal );
        TerminationSignal = true;
    };

    static auto InitializationExecution = [] {
        ErrorGuard( FCGX_Init() );
        std::atexit( OS_LibShutdown );
        std::atexit( [] { std::println( "Porgram exiting from standard exit pathway." ); } );
        std::signal( SIGINT, TerminationHandler );
        std::signal( SIGTERM, TerminationHandler );
        return 0;
    }();

    struct FileDescriptor
    {
        using NativeFileDescriptorType = decltype( FCGX_OpenSocket( {}, {} ) );
        NativeFileDescriptorType FD;

        constexpr FileDescriptor() : FD{} {}

        FileDescriptor( const std_fs::path& SocketPath )  //
            : FD{ SocketPath.empty()                      //
                      ? NativeFileDescriptorType{}
                      : FCGX_OpenSocket( SocketPath.c_str(), Config::DefaultBackLogNumber ) }
        {
            if( FD == -1 )
            {
                std::println( "Failed to open socket." );
                std::exit( -1 );
            }
            if( FD > 0 ) { std_fs::permissions( SocketPath, std_fs::perms::all ); }
        }

        constexpr operator NativeFileDescriptorType() const { return FD; }

        auto UnixSocketName() const -> std_fs::path
        {
            auto UnixAddr = sockaddr_un{};
            auto UnixAddrLen = socklen_t{ sizeof( UnixAddr ) };

            if( getsockname( FD, (sockaddr*)&UnixAddr, &UnixAddrLen ) == 0 )
            {
                switch( UnixAddr.sun_family )
                {
                    case AF_UNIX :
                        return std_fs::canonical( UnixAddr.sun_path );
                        // case AF_INET : return "0.0.0.0";
                }
            }
            return {};
        }
    };

    struct Response
    {};

    struct Request
    {
        // gcc-14's bug to issue internal linkage warning
        using FCGX_Request_Deleter_BUGGED = decltype( []( FCGX_Request* P ) {
            FCGX_Finish_r( P );
            delete P;
        } );

        // work around
        struct FCGX_Request_Deleter
        {
            static auto operator()( FCGX_Request* P )
            {
                FCGX_Finish_r( P );
                delete P;
            }
        };

        std::unique_ptr<FCGX_Request, FCGX_Request_Deleter> FCGX_Request_Ptr;

        Request() = default;
        // Request( const Request& ) = delete;
        // Request( Request&& Other ) : FCGX_Request_Ptr{ std::move( Other.FCGX_Request_Ptr ) } {};

        Request( FileDescriptor SocketFD ) : FCGX_Request_Ptr{ std::make_unique_for_overwrite<FCGX_Request>().release() }
        {
            if( FCGX_InitRequest( FCGX_Request_Ptr.get(), SocketFD, FCGI_FAIL_ACCEPT_ON_INTR ) == 0 &&  //
                FCGX_Accept_r( FCGX_Request_Ptr.get() ) == 0 )
                return;

            // fail to obtain valid request, reset residual request data & allocation
            FCGX_InitRequest( FCGX_Request_Ptr.get(), 0, 0 );
            FCGX_Request_Ptr.reset();
        }

        static auto AcceptFrom( FileDescriptor SocketFD ) { return Request{ SocketFD }; }

        auto GetParam( std::string_view ParamName ) const
        {
            const char* Result = FCGX_GetParam( std::data( ParamName ), FCGX_Request_Ptr->envp );
            if( Result == nullptr ) Result = "Param Not Found";
            return std::string_view{ Result };
        }

        auto empty() const { return FCGX_Request_Ptr == nullptr; }

        // virtual ~Request() { FCGX_Finish_r( FCGX_Request_Ptr.get() ); }
    };

    struct Server
    {
        FileDescriptor SocketFD;

        Server( FileDescriptor FD ) : SocketFD{ FD }
        {
            std::println( "Server file descriptor : {}", static_cast<int>( SocketFD ) );
            std::println( "Unix Socket Path : {}", SocketFD.UnixSocketName().c_str() );
            std::println( "Ready to accept requests..." );

            std::thread( [FD = SocketFD] {
                // todo: use CV nofity
                while( ! TerminationSignal ) std::this_thread::sleep_for( 500ms );
                FCGX_ShutdownPending();       // [os_unix.c:108] shutdownPending = TRUE;
                ::shutdown( FD, SHUT_RDWR );  // cause accept() to stop blocking
                // ::close( FD );
            } ).detach();
        }

        // Server() : Server{ FileDescriptor{} }
        Server() : Server{ FileDescriptor{ Config::OptionSocketPath.value_or( Config::DefaultSocketPath ) } } {}
        Server( const std_fs::path& SocketPath ) : Server{ FileDescriptor{ SocketPath } } {}

        auto operator=( auto&& ) = delete;

        auto NextRequest() const { return Request::AcceptFrom( SocketFD ); }

        // RequestContainer is not thread safe by default.
        // It is expected to be handled by one managing thread
        // For handling RequestContainer by multiple threads,
        // users should manage their own mutex
        struct RequestContainer
        {
            FileDescriptor SocketFD;
            Request PendingRequest;

            struct Sentinel
            {};
            struct Iterator
            {
                RequestContainer& BaseRange;
                decltype( auto ) operator*() { return std::move( BaseRange.PendingRequest ); }
                auto operator++() { return *this; }
            };

            auto begin() { return Iterator{ *this }; }
            auto end() { return Sentinel{}; }

            friend auto operator==( const Iterator& Iterator, const Sentinel& Sentinel )
            {
                // serve remaining request before exit
                auto& Queue = Iterator.BaseRange;
                auto& PendingRequest = Queue.PendingRequest;
                if( ! PendingRequest.empty() ) return false;
                if( TerminationSignal.load() ) return true;

                // start accepting new request from socket
                PendingRequest = Request::AcceptFrom( Queue.SocketFD );
                return PendingRequest.empty();
            }

            friend auto operator==( const Sentinel& Sentinel, const Iterator& Iterator ) { return Iterator == Sentinel; }
        };

        auto RequestQueue() const { return RequestContainer{ .SocketFD = SocketFD }; }

        // virtual ~Server()
        // {
        //     shutdown( SocketFD, SHUT_RDWR );
        //     ::close( SocketFD );
        // }
    };

}  // namespace EasyFCGI

#endif
