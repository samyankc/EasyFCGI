#define _EASY_FCGI_CPP
#include "../include/EasyFCGI.h"
using namespace std::chrono_literals;
using namespace std::string_literals;
using namespace std::string_view_literals;

namespace EasyFCGI
{
    // using namespace std::string_view_literals;
    // namespace FS = std::filesystem;
    // namespace RNG = std::ranges;
    // namespace VIEW = std::views;
    // using Json = nlohmann::json;
    // using StrView = std::string_view;
    using ParseUtil::operator""_FMT;

    inline namespace Concept
    {
        template<typename T>
        concept DumpingString = requires( T&& t ) {
            { t.dump() } -> std::same_as<std::string>;
        };
    }
    namespace Config
    {
        int DefaultBackLogNumber = int{ 128 };  // evetually be capped by /proc/sys/net/core/somaxconncat,ie.4096
        StrView SocketDirectoryPrefix = "/dev/shm/"sv;
        StrView SocketExtensionSuffix = ".sock"sv;

        std::vector<char*> CommandLine()
        {
            static auto CommandLineBuffer = [] -> std::vector<char> {
                constexpr auto CommandLineBufferMax = 4096uz;
                auto ReadBuffer = std::array<char, CommandLineBufferMax>{ '\0' };
                auto fin = std::fopen( "/proc/self/cmdline", "rb" );
                auto LoadedSize = std::fread( ReadBuffer.data(), sizeof( ReadBuffer[0] ), ReadBuffer.size(), fin );
                std::fclose( fin );
                if( LoadedSize == 0 ) return {};
                return std::vector( ReadBuffer.begin(), std::next( ReadBuffer.begin(), LoadedSize ) );
            }();
            static auto CommandLine = CommandLineBuffer                             //
                                      | VIEW::take( CommandLineBuffer.size() - 1 )  // trim trailing '\0'
                                      | VIEW::split( '\0' )                         //
                                      | VIEW::transform( RNG::data )                // reduce back to char*
                                      | RNG::to<std::vector>();
            return CommandLine;
        }
        FS::path DefaultSocketPath()
        {
            return FS::path( Config::SocketDirectoryPrefix ) /  //
                   FS::path( CommandLine()[0] )
                       .filename()  //
                       .replace_extension( Config::SocketExtensionSuffix );
        }
        std::optional<FS::path> LaunchOptionSocketPath()
        {
            for( auto&& [Option, OptionArg] : CommandLine() | VIEW::pairwise )
                if( Option == "-s"sv )  //
                    return OptionArg;
            return {};
        }

    }  // namespace Config

    auto TerminationSource = std::stop_source{};
    auto TerminationToken = TerminationSource.get_token();

    Response& Response::Set( HTTP::StatusCode NewValue ) & { return StatusCode = NewValue, *this; }
    Response& Response::Set( HTTP::ContentType NewValue ) & { return ContentType = NewValue, *this; }
    Response& Response::SetHeader( const std::string& Key, std::string Value ) &
    {
        Header[Key] = std::move( Value );
        return *this;
    }
    Response& Response::SetCookie( const std::string& Key, std::string Value ) &
    {
        Cookie[Key] = std::move( Value );
        return *this;
    }

    Response& Response::SetBody( std::string Value ) &
    {
        Body = std::move( Value );
        return *this;
    }
    Response& Response::Append( std::string_view Value ) &
    {
        Body += Value;
        return *this;
    }
    Response& Response::operator=( std::string Value ) & { return SetBody( std::move( Value ) ); }
    Response& Response::operator+=( std::string_view Value ) & { return Append( Value ); }

    Response& Response::SetBody( const Json& Value ) & { return SetBody( Value.dump() ); }
    Response& Response::Append( const Json& Value ) & { return Append( StrView{ Value.dump() } ); }
    Response& Response::operator=( const Json& Value ) & { return SetBody( Value ); }
    Response& Response::operator+=( const Json& Value ) & { return Append( Value ); }

    Response& Response::Reset() &
    {
        Set( HTTP::StatusCode::OK );
        Set( HTTP::Content::Text::Plain );
        Header.clear();
        Cookie.clear();
        Body.clear();
        return *this;
    }

    auto Request::Query::contains( StrView Key ) const -> bool { return Json.contains( Key ); }
    auto Request::Query::CountRepeated( StrView Key ) const -> decltype( Json.size() )
    {
        if( ! Json.contains( Key ) ) return 0;
        return Json[Key].size();
    }
    auto Request::Query::operator[]( StrView Key, std::size_t Index ) const -> std::string
    {
        if( ! Json.contains( Key ) ) return {};
        const auto& Slot = Json[Key];
        if( Slot.is_array() )
        {
            if( Index < Slot.size() )
                return Slot[Index];
            else
                return {};
        }
        if( Slot.is_string() ) return Slot.template get<std::string>();
        return Slot.dump();
    }

    auto Request::Cookie::operator[]( StrView Key ) const -> StrView
    {
        if( EnvPtr == nullptr ) return {};
        using namespace ParseUtil;
        auto CookieField = FCGX_GetParam( "HTTP_COOKIE", EnvPtr );
        if( CookieField == nullptr ) return {};
        for( auto Entry : CookieField | SplitBy( ';' ) )
            for( auto [K, V] : Entry | SplitOnceBy( '=' ) | VIEW::transform( TrimSpace ) | VIEW::pairwise )
                if( K == Key ) return V;
        return {};
    }

    static auto FullHeaderKey( StrView Key )
    {
        auto FullKey = "HTTP_{}"_FMT( Key );
        for( auto& C : FullKey )
        {
            if( C == '-' ) C = '_';
            C = std::toupper( C );
        }
        return FullKey;
    }

    auto Request::Header::operator[]( StrView Key ) const -> StrView
    {
        if( auto Result = FCGX_GetParam( FullHeaderKey( Key ).c_str(), EnvPtr ) )  //
            return Result;
        return {};
    }

    auto Request::Header::contains( StrView Key ) const -> bool  //
    {
        return FCGX_GetParam( FullHeaderKey( Key ).c_str(), EnvPtr ) != nullptr;
    }

    static auto NewFilePath( const FS::path& Path ) -> FS::path
    {
        auto ResultPath = Path;
        auto FileExtension = Path.extension().string();
        auto OriginalStem = Path.stem().string();
        do {
            auto TimeStampSuffix = ".{:%Y.%m.%d.%H.%M.%S}"_FMT( std::chrono::file_clock::now() );
            auto NewFileName = "{}{}{}"_FMT( OriginalStem, StrView{ TimeStampSuffix }.substr( 0, 24 ), FileExtension );
            ResultPath.replace_filename( NewFileName );
        } while( FS::exists( ResultPath ) );
        return ResultPath;
    }

    auto Request::Files::FileView::SaveAs( const FS::path& Path, const OverWriteOptions OverWriteOption ) const -> std::optional<FS::path>
    {
        auto ParentDir = Path.parent_path();
        if( ! FS::exists( ParentDir ) ) FS::create_directories( ParentDir );

        auto ResultPath = Path;
        if( FS::exists( ResultPath ) )  //
            switch( OverWriteOption )
            {
                case Abort :         return std::nullopt;
                case OverWrite :     break;
                case RenameOldFile : FS::rename( Path, NewFilePath( Path ) ); break;
                case RenameNewFile : ResultPath = NewFilePath( Path ); break;
            }

        auto FileFD = fopen( ResultPath.c_str(), "wb" );
        std::fwrite( ContentBody.data(), sizeof( 1 [ContentBody.data()] ), ContentBody.size(), FileFD );
        std::fclose( FileFD );

        return ResultPath;
    };

    auto Request::Files::operator[]( StrView Key, std::size_t Index ) const -> FileView
    {
        if( ! Storage.contains( Key ) ) return {};
        const auto& Slot = Storage.at( Key );
        if( Index < Slot.size() ) return Slot[Index];
        return {};
    }

    auto Request::GetParam( StrView ParamName ) const -> StrView
    {
        if( auto Result = FCGX_GetParam( std::data( ParamName ), FCGX_Request_Ptr->envp ) ) return Result;
        return {};
    }

    auto Request::AllHeaderEntries() const -> std::vector<StrView>
    {
        auto Result = std::vector<StrView>{};
        Result.reserve( 100 );
        for( auto EnvP = FCGX_Request_Ptr->envp; EnvP != nullptr && *EnvP != nullptr; ++EnvP )
        {
            Result.push_back( *EnvP );
        }
        return Result;
    }

    auto Request::Parse() -> int
    {
        using namespace ParseUtil;
        Query.Json = {};  // .clear() cannot reset residual discarded state
        Files.Storage.clear();
        Payload.clear();

        Header.EnvPtr = FCGX_Request_Ptr->envp;
        Cookie.EnvPtr = FCGX_Request_Ptr->envp;

        Method = GetParam( "REQUEST_METHOD" );
        ContentType = GetParam( "CONTENT_TYPE" );

        Payload.resize_and_overwrite( ( GetParam( "CONTENT_LENGTH" ) | ConvertTo<int> | FallBack( 0 ) ) + 1,  //
                                      [Stream = FCGX_Request_Ptr->in]( char* Buffer, std::size_t N ) {        //
                                          return FCGX_GetStr( Buffer, N, Stream );
                                      } );

        auto QueryAppend = [&Result = Query.Json]( std::string_view Key, auto&& Value ) {
            if( ! Key.empty() ) Result[Key].push_back( std::forward<decltype( Value )>( Value ) );
        };

        {
            // read query string, then request body
            // duplicated key-value will be overwritten
            for( auto Segment : GetParam( "QUERY_STRING" ) | SplitBy( '&' ) )
                for( auto [EncodedKey, EncodedValue] : Segment | SplitOnceBy( '=' ) | VIEW::pairwise )
                    QueryAppend( DecodeURLFragment( EncodedKey ), DecodeURLFragment( EncodedValue ) );

            switch( ContentType )
            {
                default : break;
                case HTTP::Content::Application::FormURLEncoded :
                {
                    for( auto Segment : Payload | SplitBy( '&' ) )
                        for( auto [EncodedKey, EncodedValue] : Segment | SplitOnceBy( '=' ) | VIEW::pairwise )
                            QueryAppend( DecodeURLFragment( EncodedKey ), DecodeURLFragment( EncodedValue ) );
                    break;
                }
                case HTTP::Content::MultiPart::FormData :
                {
                    // auto _ = ScopedTimer( "MultipartParseTime" );
                    auto BoundaryPattern = GetParam( "CONTENT_TYPE" ) | After( "boundary=" ) | TrimSpace;
                    auto ExtendedBoundaryPattern = "\r\n--{}\r\nContent-Disposition: form-data; name="_FMT( BoundaryPattern );

                    // remove trailing boundary to avoid empty ending after split
                    auto PayloadView = Payload | TrimSpace | TrimTrailing( "--" ) | TrimTrailing( BoundaryPattern ) | TrimTrailing( "\r\n--" );
                    if( PayloadView.empty() ) break;

                    auto NameFieldPrefix = StrView{ "name=" };
                    if( PayloadView.starts_with( ExtendedBoundaryPattern.substr( 2 ) ) )
                    {  // enable ExtendedBoundaryPattern
                        BoundaryPattern = ExtendedBoundaryPattern;
                        NameFieldPrefix = NameFieldPrefix | CollapseToEnd;
                    }
                    else
                    {  // fallback to standard
                        BoundaryPattern = ExtendedBoundaryPattern | Before( "Content-Disposition" );
                    }

                    PayloadView = PayloadView | TrimLeading( BoundaryPattern.substr( 2 ) );

                    // at this point, PayloadView does not contain BoundaryPattern on both ends.

                    for( auto&& Body : PayloadView | SplitBy( BoundaryPattern ) )
                    {
                        auto [Header, Content] = Body | SplitOnceBy( "\r\n\r\n" );

                        auto Name = Header | After( NameFieldPrefix ) | Between( '"' );
                        if( Name.empty() ) break;  // should never happen?
                        auto FileName = Header | After( "filename=" ) | Between( '"' );
                        auto ContentType = Header | After( "\r\n" ) | After( "Content-Type:" ) | TrimSpace;

                        if( ContentType.empty() )
                        {
                            QueryAppend( Name, Content );
                        }
                        else
                        {
                            QueryAppend( Name, FileName );
                            if( ! FileName.empty() || ! Content.empty() )  //
                                Files.Storage[Name].emplace_back( FileName, ContentType, Content );
                        }
                    }
                    break;
                }
                case HTTP::Content::Application::Json :
                {
                    Query.Json = Json::parse( Payload, nullptr, false );  // disable exception
                    if( Query.Json.is_discarded() )
                    {  // parse error
                        // early response with error message
                        // caller does not see this iteration
                        // give caller the next request
                        FCGX_PutS( "Status: 400\r\n"
                                   "Content-Type: text/html; charset=UTF-8\r\n"
                                   "\r\n"
                                   "Invalid Json.",
                                   FCGX_Request_Ptr->out );

                        std::println( "Responding 400 Bad Request to Request with invalid Json.\nReady to accept new request..." );

                        // re-using FCGX_Request struct, parse again
                        if( FCGX_Accept_r( FCGX_Request_Ptr.get() ) == 0 ) return Parse();
                        return -1;
                    }
                    break;
                }
            }
        }
        return 0;
    }

    Request::Request() = default;
    Request::Request( Request&& Other ) = default;
    // Request::Request( const Request& ) = delete;

    Request::Request( SocketFileDescriptor SocketFD )  //
        : FCGX_Request_Ptr{ std::make_unique_for_overwrite<FCGX_Request>().release() }
    {
        auto Request_Ptr = FCGX_Request_Ptr.get();
        (void)FCGX_InitRequest( Request_Ptr, SocketFD, FCGI_FAIL_ACCEPT_ON_INTR );
        Request_Ptr->keepConnection = 0;  // disable fastcgi_keep_conn
        if( FCGX_Accept_r( Request_Ptr ) == 0 )
        {
            // FCGX_Request_Ptr ready, setup the rest of request object(parse request)
            if( Parse() == 0 ) return;
        }

        // fail to obtain valid request, reset residual request data & allocation
        // custom deleter of FCGX_Request_Ptr will do the cleanup
        FCGX_InitRequest( Request_Ptr, {}, {} );
        FCGX_Request_Ptr.reset();

        if( TerminationToken.stop_requested() ) std::println( "Interrupted FCGX_Accept_r()." );
    }

    Request& Request::operator=( Request&& Other ) & = default;

    auto Request::empty() const -> bool { return FCGX_Request_Ptr == nullptr; }

    auto Request::operator[]( StrView Key, std::size_t Index ) const -> decltype( Query[{}, {}] ) { return Query[Key, Index]; }

    static auto OutputIteratorFor( const std::unique_ptr<FCGX_Request>& FCGX_Request_Ptr )
    {
        struct OutIt
        {
            using difference_type = std::ptrdiff_t;
            FCGX_Stream* Out;
            auto operator*() const { return *this; }
            auto& operator++() & { return *this; }
            auto operator++( int ) const { return *this; }
            auto operator=( char C ) const { return FCGX_PutChar( C, Out ); }
        };
        return OutIt{ FCGX_Request_Ptr->out };
    }

    auto Request::Send( StrView Content ) const -> void
    {
        if( Content.empty() || FCGX_Request_Ptr == nullptr ) return;
        FCGX_PutStr( Content.data(), Content.length(), FCGX_Request_Ptr->out );
    }

    auto Request::SendLine( StrView Content ) const -> void
    {
        Send( Content );
        Send( "\r\n" );
    }

    // template<typename... Args>
    // requires( sizeof...( Args ) > 0 )
    // auto Request::Send( const std::format_string<Args...>& fmt, Args&&... args ) const  //
    // {
    //     if( FCGX_Request_Ptr == nullptr ) return;
    //     std::format_to( OutputIteratorFor( FCGX_Request_Ptr ), fmt, std::forward<Args>( args )... );
    // }

    // template<typename... Args>
    // requires( sizeof...( Args ) > 0 )
    // auto Request::SendLine( const std::format_string<Args...>& fmt, Args&&... args ) const  //
    // {
    //     Send( fmt, std::forward<Args>( args )... );
    //     SendLine();
    // }

    auto Request::FlushHeader() -> HTTP::StatusCode
    {
        using enum HTTP::StatusCode;
        switch( Response.StatusCode )
        {
            case InternalUse_HeaderAlreadySent : return InternalUse_HeaderAlreadySent;
            case HTTP::StatusCode::NoContent :   SendLine( "Status: 204" ); break;
            default :
                SendLine( "Status: {}"_FMT( std::to_underlying( Response.StatusCode ) ) );
                SendLine( "Content-Type: {}; charset=UTF-8"_FMT( Response.ContentType.EnumLiteral() ) );
                break;
        }

        for( auto&& [K, V] : Response.Cookie ) SendLine( "Set-Cookie: {}={}"_FMT( K, V ) );
        for( auto&& [K, V] : Response.Header ) SendLine( "{}: {}"_FMT( K, V ) );
        SendLine();
        FCGX_FFlush( FCGX_Request_Ptr->out );

        return std::exchange( Response.StatusCode, InternalUse_HeaderAlreadySent );
    }

    auto Request::FlushResponse() -> decltype( FCGX_FFlush( {} ) )
    {
        Send( Response.Body );
        Response.Body.clear();
        return FCGX_FFlush( FCGX_Request_Ptr->out );
    }

    auto Request::EarlyFinish() -> void { std::exchange( *this, {} ); }

    auto Request::SSE_Start() -> void
    {
        if( Response.StatusCode == HTTP::StatusCode::InternalUse_HeaderAlreadySent )
        {
            std::println( "[ Fail ] Attempting SSE_Start after header flushed. no-op." );
            return;
        }
        Response
            .Set( HTTP::StatusCode::OK )  //
            .Set( HTTP::Content::Text::EventStream )
            .SetHeader( "Cache-Control", "no-cache" );
        FlushHeader();
        FlushResponse();
    }

    auto Request::SSE_Error() const -> decltype( FCGX_GetError( {} ) ) { return FCGX_GetError( FCGX_Request_Ptr->out ); }

    auto Request::Dump() const -> std::string
    {
        auto Result = std::string{};
        Result += std::format( "Method: [{}]\n", Method );
        Result += std::format( "Content Type: [{}]\n", ContentType );
        Result += "Header: [\n";
        for( auto E : AllHeaderEntries() ) Result += std::format( "  {}\n", E );
        Result += "]\n";

        Result += std::format( "Query String: [{}]\n", GetParam( "QUERY_STRING" ) );
        if( Payload.empty() )
            Result += "Payload: [{}]\n";
        else
            Result += std::format( "Payload: [\n{}]\n", Payload );

        Result += std::format( "{:-^50}\n", "" );

        if( Query.Json.empty() )
            Result += "Query Json: []\n";
        else
            Result += std::format( "Query Json: [\n{}\n]\n", Query.Json.dump( 2 ) );

        if( Files.Storage.empty() )
            Result += "Files: []\n";
        else
            Result += std::format( "Files: [{}]\n",                                                     //
                                   RNG::fold_left( Files.Storage, "\n", []( auto&& Acc, auto&& New ) {  //
                                       auto&& [K, V] = New;
                                       auto List = std::string{};
                                       for( auto&& F : V ) List = std::format( "{}{}, ", List, F.FileName );
                                       return std::format( "{}{}:{}\n", Acc, K, List );
                                   } ) );

        return std::format( "{0:-^50}\n\n{1}\n{0:-^50}", "Request Detail", Result );
    }

    Request::~Request()
    {
        if( FCGX_Request_Ptr == nullptr ) return;
        // std::println( "ID: [ {:2},{:2} ] Request Complete...", FCGX_Request_Ptr->ipcFd, FCGX_Request_Ptr->requestId );
        if( FlushHeader() != HTTP::StatusCode::NoContent ) FlushResponse();
        FCGX_Request_Ptr->keepConnection = 0;  // just in case
        FCGX_Finish_r( FCGX_Request_Ptr.get() );
        FCGX_Request_Ptr.reset();
    }

    static auto UnixSocketName( SocketFileDescriptor FD ) -> FS::path
    {
        auto UnixAddr = sockaddr_un{};
        auto UnixAddrLen = socklen_t{ sizeof( UnixAddr ) };

        if( getsockname( FD, (sockaddr*)&UnixAddr, &UnixAddrLen ) == 0 )
        {
            switch( UnixAddr.sun_family )
            {
                case AF_UNIX :
                    return FS::canonical( UnixAddr.sun_path );
                    // case AF_INET : return "0.0.0.0";
            }
        }
        return {};
    }

    extern "C" void OS_LibShutdown();  // for omitting #include <fcgios.h>
    auto ServerInitialization() -> void
    {
        static auto ServerInitializationComplete = false;
        if( std::exchange( ServerInitializationComplete, true ) ) return;

        if( auto ErrorCode = FCGX_Init(); ErrorCode == 0 )
        {
            std::println( "[ OK ]  ServerInitialization : FCGX_Init" );
            std::atexit( OS_LibShutdown );

            struct sigaction SignalAction;
            sigemptyset( &SignalAction.sa_mask );
            SignalAction.sa_flags = 0;  // disable SA_RESTART
            SignalAction.sa_handler = []( int Signal ) {
                TerminationSource.request_stop();
                FCGX_ShutdownPending();
            };
            ::sigaction( SIGINT, &SignalAction, nullptr );
            ::sigaction( SIGTERM, &SignalAction, nullptr );

            // sigemptyset( &SignalAction.sa_mask );
            // SignalAction.sa_handler = SIG_IGN;
            // ::sigaction( SIGPIPE, &SignalAction, nullptr );
        }
        else
        {
            std::println( "[ Fatal, Error = {} ]  ServerInitialization : FCGX_Init NOT Successful", ErrorCode );
            std::exit( ErrorCode );
        }
    };

    Server::RequestQueue::RequestQueue( SocketFileDescriptor SourceSocketFD ) : ListenSocket{ SourceSocketFD } {};

    // static auto PollFor( int FD, unsigned int Flags, int Timeout = -1 )
    // {
    //     auto PollFD = pollfd{ .fd = FD, .events = static_cast<short>( Flags ), .revents = 0 };
    //     return ::poll( &PollFD, 1, Timeout ) > 0 && ( PollFD.revents & Flags ) == Flags;
    // }
    auto Server::RequestQueue::WaitForListenSocket( int Timeout ) const -> bool
    {
        constexpr unsigned int Flags = POLLIN;
        auto PollFD = pollfd{ .fd = ListenSocket, .events = static_cast<short>( Flags ), .revents = {} };
        return ::poll( &PollFD, 1, Timeout ) > 0 && ( PollFD.revents & Flags ) == Flags;

        // return PollFor( ListenSocket, POLLIN, Timeout );
    }
    auto Server::RequestQueue::ListenSocketActivated() const -> bool { return WaitForListenSocket( 0 ); }
    auto Server::RequestQueue::NextRequest() const -> Request { return Request{ ListenSocket }; }
    auto Server::RequestQueue::Iterator::operator++() & -> Iterator& { return *this; }  //no-op
    auto Server::RequestQueue::Iterator::operator*() const -> Request { return AttachedQueue.ListenSocketActivated() ? AttachedQueue.NextRequest() : Request{}; }
    auto Server::RequestQueue::Iterator::operator==( Sentinel ) const -> bool
    {
        // auto _ = ScopedTimer( "WaitForListenSocket" );
        return ! AttachedQueue.WaitForListenSocket();
    }

    auto Server::RequestQueue::begin() const -> Iterator { return { *this }; }
    auto Server::RequestQueue::end() const -> Sentinel { return {}; }

    // this is THE primary constructor
    Server::Server( SocketFileDescriptor ListenSocket ) : RequestQueue{ ListenSocket }
    {
        ServerInitialization();
        std::println( "Server file descriptor : {}", static_cast<int>( ListenSocket ) );
        std::println( "Unix Socket Path : {}", UnixSocketName( ListenSocket ).c_str() );
        std::println( "Ready to accept requests..." );
    }

    // Server::Server() : Server{ SocketFileDescriptor{} }{}
    Server::Server() : Server( Config::LaunchOptionSocketPath().value_or( Config::DefaultSocketPath() ) ) {}
    Server::Server( const FS::path& SocketPath )
        : Server( SocketPath.empty()  //
                      ? SocketFileDescriptor{}
                      : FCGX_OpenSocket( SocketPath.c_str(), Config::DefaultBackLogNumber ) )
    {
        auto FD = RequestQueue.ListenSocket;
        if( FD == -1 )
        {
            std::println( "Failed to open socket." );
            std::exit( -1 );
        }
        if( FD > 0 )
        {
            FS::permissions( SocketPath, FS::perms::all );
        }
    }

}  // namespace EasyFCGI
