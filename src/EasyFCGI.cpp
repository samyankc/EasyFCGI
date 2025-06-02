#include "EasyFCGI/EasyFCGI.h"
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>
#include <array>

//explicit template instantiation
template struct std::formatter<HTTP::RequestMethod>;
template struct std::formatter<HTTP::ContentType>;
template struct ParseUtil::ConvertToRA<int, 10>;
template struct ParseUtil::FallBack<int>;
template const ParseUtil::ConvertToRA<int, 10> ParseUtil::ConvertTo<int>;

namespace
{
    auto ReadContent( const std::filesystem::path& FilePath, std::size_t Capacity = 1024uz ) -> std::vector<char>
    {
        auto Buffer = std::vector<char>{};
        if( auto File = std::fopen( FilePath.c_str(), "rb" ) )
        {
            Buffer.resize( Capacity, '\0' );
            Buffer.resize( std::fread( Buffer.data(), sizeof( char ), Buffer.size(), File ) );
            std::fclose( File );
        }
        return Buffer;
    }

}  // namespace

namespace glz
{
    bool operator==( const json_t& LHS, const json_t& RHS ) { return LHS.data == RHS.data; }
    bool operator==( const raw_json& LHS, const raw_json& RHS ) { return LHS.str == RHS.str; }
}  // namespace glz

namespace EasyFCGI
{
    using ParseUtil::operator""_FMT;

    inline namespace Concept
    {
        template<typename T>
        concept DumpingString [[deprecated]] = requires( T&& t ) {
            { t.dump() } -> std::same_as<std::string>;
        };
    }
    namespace Config
    {
        int DefaultBackLogNumber = int{ 128 };  // evetually be capped by /proc/sys/net/core/somaxconncat,ie.4096

        auto CommandLine()
        {
            static auto CommandLineBuffer = ReadContent( "/proc/self/cmdline" );
            static auto CommandLine = CommandLineBuffer                         //
                                      | VIEW::take( CommandLineBuffer.size() )  // keep trailing '\0'
                                      | VIEW::split( '\0' )                     // will result in ending empty element
                                      | VIEW::transform( RNG::data )            // reduce back to char*
                                      | RNG::to<std::vector>();
            CommandLine.back() = &CommandLineBuffer.back();  // end argv with "\0"
            return CommandLine;
        }

        bool RunAsDaemon = LaunchOptionContains( "d" );
        auto ScriptName = FS::path( CommandLine()[0] ).filename().replace_extension( {} ).string();

        FS::path CWD{};
        FS::path LogFilePath{};
        FS::path PidFilePath{};
        FS::path SokcetPath{};

        auto LaunchOptionContains( StrView TargetOption ) -> bool
        {
            for( StrView Option : CommandLine() )
            {
                if( Option.starts_with( "--" ) )
                {
                    Option.remove_prefix( 2 );
                    if( Option == TargetOption ) return true;
                }
                else if( Option.starts_with( '-' ) )
                {
                    Option.remove_prefix( 1 );
                    if( Option.contains( TargetOption ) ) return true;
                }
            }
            return false;
        }

        auto LaunchOptionValue( StrView TargetOption ) -> std::optional<StrView>
        {
            for( auto&& [OptionKey, OptionValue] : CommandLine() | VIEW::pairwise )
            {
                auto Option = StrView( OptionKey );
                if( Option.starts_with( "--" ) )
                    Option.remove_prefix( 2 );
                else if( Option.starts_with( '-' ) )
                    Option.remove_prefix( 1 );
                else
                    continue;
                if( ! Option.starts_with( TargetOption ) ) continue;

                Option.remove_prefix( TargetOption.length() );
                if( Option.empty() )
                {
                    if( OptionValue[0] == '-' ) return std::nullopt;
                    return OptionValue;
                }
                else if( Option[0] == '=' )
                {
                    Option.remove_prefix( 1 );
                    if( Option.empty() ) return std::nullopt;
                    return Option;
                }
            }
            return std::nullopt;
        }

    }  // namespace Config

    auto DumpJson( const EasyFCGI::Json& J ) -> std::string
    {
        if( auto TryDump = J.dump() )
        {
            return std::move( TryDump.value() );
        }
        else
        {
            return std::format( R"({{"DumpError":"{}"}})", TryDump.error().custom_error_message );
        }
    }

    auto TerminationSource = std::stop_source{};
    std::stop_token TerminationToken = TerminationSource.get_token();

    namespace SleepFor_Helper
    {
        thread_local auto CVA = std::condition_variable_any{};
        thread_local auto DummyMutex = std::mutex{};
        thread_local auto DummyLock = std::unique_lock{ DummyMutex };
    }

    auto SleepFor( Clock::duration SleepDuartion ) -> bool
    {
        using namespace SleepFor_Helper;
        CVA.wait_for( DummyLock, TerminationToken, SleepDuartion, std::false_type{} );
        return ! TerminationToken.stop_requested();
    }

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

    Response& Response::SetBody( const char* Value ) & { return SetBody( std::string{ Value } ); }
    Response& Response::Append( const char* Value ) & { return Append( StrView{ Value } ); }

    Response& Response::SetBody( const Json& Value ) & { return SetBody( DumpJson( Value ) ); }
    Response& Response::Append( const Json& Value ) & { return Append( StrView{ DumpJson( Value ) } ); }

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

    auto Request::Query::StringDump( const EasyFCGI::Json& Json ) -> std::string  //
    {
        return Json.is_string() ? Json.get_string() : DumpJson( Json );
    }
    auto Request::Query::GetOptional( StrView Key, std::size_t Index ) const -> std::optional<std::string>
    {
        if( ! Json.contains( Key ) ) return std::nullopt;
        const auto& Slot = Json[Key];
        if( Slot.is_array() && Slot.size() > Index ) return StringDump( Slot[auto{ Index }] );
        if( Index > 0 ) return std::nullopt;
        return StringDump( Slot );
    }
    auto Request::Query::operator[]( StrView Key, std::size_t Index ) const -> std::string  //
    {
        return GetOptional( Key, Index ).value_or( std::string{} );
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
        if( FCGX_Request_Ptr == nullptr ) return {};
        if( auto Result = FCGX_GetParam( std::data( ParamName ), FCGX_Request_Ptr->envp ) ) return Result;
        return {};
    }

    auto Request::AllHeaderEntries() const -> std::vector<StrView>
    {
        auto Result = std::vector<StrView>{};
        if( FCGX_Request_Ptr == nullptr ) return Result;
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
        Query.Json.reset();
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
            if( Key.empty() ) return;
            if( ! Result.contains( Key ) || ! Result[Key].is_array() ) Result[Key] = Json::array_t{};
            Result[Key].get_array().push_back( std::forward<decltype( Value )>( Value ) );
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

                    // remark: by RFC 2046, boundary is at most 70 character long
                    //         which means ExtendedBoundaryPatternbuffer size is at most 113
                    constexpr auto BoundaryLengthLimit = 70uz;
                    auto BoundaryPattern = GetParam( "CONTENT_TYPE" ) | After( "boundary=" ) | TrimSpace;
                    if( BoundaryPattern.length() > BoundaryLengthLimit ) break;

                    // remove trailing boundary to avoid empty ending after split
                    auto PayloadView = Payload | TrimSpace | TrimTrailing( "--" ) | TrimTrailing( BoundaryPattern ) | TrimTrailing( "\r\n--" );
                    if( PayloadView.empty() ) break;

                    constexpr auto ExtendedBoundaryFormatString = StrView{ "\r\n--{}\r\nContent-Disposition: form-data; name=" };
                    char ExtendedBoundaryBuffer[BoundaryLengthLimit + ExtendedBoundaryFormatString.length() - 2];
                    auto ExtendedBoundaryFormatResult = std::format_to_n( ExtendedBoundaryBuffer, std::size( ExtendedBoundaryBuffer ),  //
                                                                          ExtendedBoundaryFormatString, BoundaryPattern );
                    auto ExtendedBoundary = StrView{ ExtendedBoundaryBuffer, ExtendedBoundaryFormatResult.out };

                    PayloadView = PayloadView | TrimLeading( ExtendedBoundary.substr( 2 ) );

                    // at this point, PayloadView does not contain ExtendedBoundary on both ends.

                    for( auto&& PayloadFragment : PayloadView | SplitBy( ExtendedBoundary ) )
                    {
                        auto [Header, Content] = PayloadFragment | SplitOnceBy( "\r\n\r\n" );

                        auto Name = Header | Between( '"' );
                        auto FileName = Header | After( "filename=" ) | Between( '"' );
                        auto ContentType = Header | After( "\r\n" ) | After( "Content-Type:" ) | TrimSpace;

                        if( Name.empty() ) break;  // should never happen?
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
                    auto ParseResult = glz::read_json( Query.Json, Payload );
                    if( ParseResult != glz::error_code::none )
                    {  // parse error
                        // early response with error message
                        // caller does not see this iteration
                        // give caller the next request
                        FCGX_PutS( "Status: 400\r\n"
                                   "Content-Type: text/html; charset=UTF-8\r\n"
                                   "\r\n"
                                   "Invalid Json.",
                                   FCGX_Request_Ptr->out );

                        std::println( "{}\n", ParseResult.custom_error_message );
                        std::println( "Responding 400 Bad Request to Request with invalid Json.\nReady to accept new request..." );
                        std::fflush( stdout );
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
        std::ignore = FCGX_InitRequest( Request_Ptr, SocketFD, FCGI_FAIL_ACCEPT_ON_INTR );
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
        if( FCGX_Request_Ptr == nullptr ) Result += "[Dump] This request object is not attached to an actual request.\n";
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
            Result += std::format( "Query Json: [\n{}\n]\n", glz::write<glz::opts{ .prettify = true }>( Query.Json ).value_or( "{}" ) );

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
        std::fflush( stdout );  // flush log
        std::fflush( stderr );
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

    namespace ConfigureIO
    {
        constexpr auto Coalesce = []( FS::path& P, FS::path&& F ) static -> const FS::path& {
            if( P.empty() ) P = F;
            return P;
        };
        constexpr auto Prepare = []( const FS::path& P ) static { FS::create_directories( P.parent_path() ); };
        constexpr auto ToPath = []( StrView SV ) static { return FS::path( SV ); };

        static void ChangeDir()
        {
            FS::create_directories( Coalesce( Config::CWD, Config::LaunchOptionValue( "dir" ).transform( ToPath ).value_or( FS::current_path() ) ) );
            auto EC = std::error_code{};
            FS::current_path( Config::CWD, EC );
            if( ! EC ) return;
            std::println( "[ Fatal ] Fail to change working directory\n[ Error {} ] {}", EC.value(), EC.message() );
            std::exit( EC.value() );
        }

        static void PrepareSocket()
        {
            Prepare( Coalesce( Config::SokcetPath, Config::LaunchOptionValue( "socket" ).transform( ToPath ).value_or( Config::CWD / "run" / ( Config::ScriptName + ".sock" ) ) ) );
            /// TODO:
            // check if socket is in use
            // open socket and try listen
            // or maybe exit()?
        }

        static void Daemonize()
        {
            std::println( "Application will run as daemon." );
            auto Error = ::daemon( true, true );  // is it necessary to do double fork?
            if( Error == -1 )
            {
                std::println( "[ Error {} ] {}\n Fail to daemonize", errno, strerrordesc_np( errno ) );
                exit( errno );
            }
        }

        static void PrepareIOFiles()
        {
            Prepare( Coalesce( Config::LogFilePath, Config::LaunchOptionValue( "log" ).transform( ToPath ).value_or( Config::CWD / "log" / ( Config::ScriptName + ".log" ) ) ) );
            Prepare( Coalesce( Config::PidFilePath, Config::LaunchOptionValue( "pid" ).transform( ToPath ).value_or( Config::CWD / "run" / ( Config::ScriptName + ".pid" ) ) ) );
        }

        static void RedirectIO()
        {
            // shutdown previous stale process using pid file
            auto PidFilePathStr = Config::PidFilePath.c_str();
            if( FS::exists( Config::PidFilePath ) )
            {
                if( auto PID = ReadContent( Config::PidFilePath ).data() | ConvertTo<int> | ParseUtil::FallBack( 0 ) )
                {
                    auto EXE = FS::path( "/proc/{}/exe"_FMT( PID ) );
                    if( FS::exists( EXE ) && FS::is_symlink( EXE ) &&  //
                        StrView{ FS::read_symlink( EXE ).c_str() }.starts_with( FS::read_symlink( "/proc/self/exe" ).c_str() ) )
                    {
                        ::kill( PID, SIGTERM );
                        std::println( "Previous Daemon Process Detected. Terminating." );
                        auto Retry = 5;
                        while( FS::exists( EXE ) )
                        {
                            if( --Retry <= 0 ) ::kill( PID, SIGKILL );
                            std::println( "Waiting previous daemon process to terminate." );
                            std::fflush( stdout );
                            std::this_thread::sleep_for( 200ms );
                        }
                        std::println( "Terminated." );
                    }
                }
            }

            // IMPORTANT : the following shall NOT happen before fork() / daemon()
            auto PidFile = std::fopen( PidFilePathStr, "w" );
            std::println( PidFile, "{}", getpid() );
            std::fclose( PidFile );

            if( std::freopen( "/dev/null", "r", stdin ) == nullptr )
            {
                std::println( "[ Error {} ] {}\n Fail to redirect stdin to /dev/null", errno, strerrordesc_np( errno ) );
                exit( errno );
            }

            auto LogFilePathStr = Config::LogFilePath.c_str();
            // auto LogFileFD = ::open( LogFilePathStr, O_RDWR | O_CREAT | O_APPEND | O_NOCTTY, S_IRUSR | S_IWUSR );
            auto LogFile = std::freopen( LogFilePathStr, "a+", stdout );
            auto ErrFile = std::freopen( LogFilePathStr, "a+", stderr );
            if( LogFile == nullptr || ErrFile == nullptr )
            {
                std::println( "[ Error {} ] {}\n Fail to open log/err file : {}\nRedirect IO to /dev/null", errno, strerrordesc_np( errno ), LogFilePathStr );
                if( std::freopen( "/dev/null", "a+", stdout ) == nullptr ||  //
                    std::freopen( "/dev/null", "a+", stderr ) == nullptr )
                {
                    std::println( "[ Error {} ] {}\n Fail to redirect stdout/stderr to /dev/null", errno, strerrordesc_np( errno ) );
                    exit( errno );
                }
            }

            std::fflush( stdout );
        }

        static void RunSequence()
        {
            ChangeDir();
            PrepareSocket();
            if( Config::RunAsDaemon )
            {
                Daemonize();
                PrepareIOFiles();
                RedirectIO();
            }
        }
    }  // namespace ConfigureIO

    extern "C" void OS_LibShutdown();  // for omitting #include <fcgios.h>
    static auto ServerInitialization() -> void
    {
        static auto ServerInitializationComplete = false;
        if( std::exchange( ServerInitializationComplete, true ) ) return;

        ConfigureIO::RunSequence();

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
            ::sigaction( SIGHUP, &SignalAction, nullptr );
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

    auto Server::RequestQueue::WaitForListenSocket( int Timeout ) const -> bool
    {
        constexpr short Flags = POLLIN;
        auto PollFD = pollfd{ .fd = ListenSocket, .events = Flags, .revents = {} };
        return ::poll( &PollFD, 1, Timeout ) > 0 && ( PollFD.revents & Flags ) == Flags;
    }
    auto Server::RequestQueue::ListenSocketActivated() const -> bool { return WaitForListenSocket( 0 ); }
    auto Server::RequestQueue::NextRequest() const -> Request { return Request{ ListenSocket }; }
    auto Server::RequestQueue::Iterator::operator++() & -> Iterator& { return *this; }  //no-op
    auto Server::RequestQueue::Iterator::operator*() const -> Request { return AttachedQueue.ListenSocketActivated() ? AttachedQueue.NextRequest() : Request{}; }
    auto Server::RequestQueue::Iterator::operator==( Sentinel ) const -> bool
    {
        // auto _ = ScopedTimer( "WaitForListenSocket" );
        return TerminationToken.stop_requested() || ! AttachedQueue.WaitForListenSocket();
    }

    auto Server::RequestQueue::begin() const -> Iterator { return { *this }; }
    auto Server::RequestQueue::end() const -> Sentinel { return {}; }

    // this is THE primary constructor
    Server::Server( SocketFileDescriptor ListenSocket ) : RequestQueue{ ListenSocket }
    {
        ServerInitialization();
        std::println( "Server file descriptor : {}", static_cast<int>( ListenSocket ) );
        std::println( "Unix Socket Path : {}", UnixSocketName( ListenSocket ).c_str() );
        std::println( "Log File Path : {}", Config::LogFilePath.c_str() );
        std::println( "PID File Path : {}", Config::PidFilePath.c_str() );
        std::println( "Ready to accept requests..." );
        std::fflush( stdout );
    }

    /// TODO: rework constructor set design, it is shit now
    // Server::Server() : Server{ SocketFileDescriptor{} }{}
    Server::Server() : Server(( ServerInitialization(), Config::SokcetPath )) {}
    Server::Server( const FS::path& SocketPath )
        : Server( SocketPath.empty()  //
                      ? SocketFileDescriptor{}
                      : FCGX_OpenSocket( SocketPath.c_str(), Config::DefaultBackLogNumber ) )
    {
        auto FD = RequestQueue.ListenSocket;
        if( FD == -1 )
        {
            std::println( "[ Fatal ] Failed to open socket." );
            std::exit( -1 );
        }
        if( FD > 0 )
        {
            FS::permissions( SocketPath, FS::perms::all );
        }
    }

}  // namespace EasyFCGI