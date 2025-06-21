#ifndef _EASY_FCGI_H
#define _EASY_FCGI_H

// #ifdef GLZ_NULL_TERMINATED
// #undef GLZ_NULL_TERMINATED
// #endif
// #define GLZ_NULL_TERMINATED false

#include <fcgiapp.h>
#include <csignal>
#include <cstddef>
#include <memory>
#include <concepts>
#include <utility>
#include <vector>
#include <ranges>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <chrono>
#include <print>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <thread>
#include <stop_token>
#include <flat_map>
#include <cstdint>
#include "glaze/glaze.hpp"

namespace glz
{
    bool operator==( const json_t&, const json_t& );
    bool operator==( const raw_json&, const raw_json& );
}

using namespace std::chrono_literals;
using namespace std::string_literals;
using namespace std::string_view_literals;

namespace ParseUtil
{
    namespace RNG = std::ranges;
    namespace VIEW = std::views;
    using StrView = std::string_view;

    template<std::size_t N>
    struct FixedString
    {
        char Data[N + 1]{};
        consteval FixedString( const char ( &Src )[N] ) noexcept { std::copy_n( Src, N, Data ); }
        consteval auto data() const noexcept { return Data; }
        consteval std::size_t size() const noexcept { return N - ( Data[N - 1] == '\0' ); }
        consteval operator StrView() const noexcept { return { Data, size() }; }
    };

    template<FixedString FSTR>
    constexpr auto operator""_FMT() noexcept
    {
        return []( auto&&... args ) { return std::format( FSTR, std::forward<decltype( args )>( args )... ); };
    }

    struct StrViewPattern
    {
        constexpr static auto ASCII = [] {
            auto ASCII = std::array<char, 256>{};
            RNG::iota( ASCII, 0 );
            return ASCII;
        }();

        constexpr StrViewPattern() : Pattern{} {};
        constexpr StrViewPattern( const StrViewPattern& ) = default;
        constexpr StrViewPattern( std::convertible_to<StrView> auto&& OtherPattern ) : Pattern{ OtherPattern } {};
        constexpr StrViewPattern( char CharPattern ) : Pattern{ &ASCII[CharPattern], 1 } {}
        // constexpr operator StrView() const { return Pattern; }
        constexpr bool operator==( const StrViewPattern& ) const = default;

      protected:
        StrView Pattern;
    };

    inline namespace RangeAdaptor
    {
        [[maybe_unused]] constexpr struct CollapseToEndRA : RNG::range_adaptor_closure<CollapseToEndRA>
        {
            constexpr auto static operator()( StrView Input ) { return StrView{ Input.end(), Input.end() }; }
        } CollapseToEnd;

        [[maybe_unused]] constexpr struct FrontRA : RNG::range_adaptor_closure<FrontRA>
        {
            constexpr auto static operator()( auto&& Range ) { return *RNG::begin( Range ); }
        } Front;

        [[maybe_unused]] constexpr struct BeginRA : RNG::range_adaptor_closure<BeginRA>
        {
            constexpr auto static operator()( auto&& Range ) { return RNG::begin( Range ); }
        } Begin;

        [[maybe_unused]] constexpr struct EndRA : RNG::range_adaptor_closure<EndRA>
        {
            constexpr auto static operator()( auto&& Range ) { return RNG::end( Range ); }
        } End;

        [[maybe_unused]] constexpr struct BoundaryRA : RNG::range_adaptor_closure<BoundaryRA>
        {
            constexpr auto static operator()( auto&& Range ) { return std::array{ RNG::begin( Range ), RNG::end( Range ) }; }
        } Boundary;

        [[maybe_unused]] constexpr struct TrimSpaceRA : RNG::range_adaptor_closure<TrimSpaceRA>
        {
            constexpr static StrView operator()( StrView Input )
            {
                auto SpaceRemoved = Input                                            //
                                    | VIEW::drop_while( ::isspace ) | VIEW::reverse  //
                                    | VIEW::drop_while( ::isspace ) | VIEW::reverse;
                return { &*RNG::begin( SpaceRemoved ),  //
                         &*RNG::rbegin( SpaceRemoved ) + 1 };
            }
        } TrimSpace;

        struct TrimLeading : StrViewPattern, RNG::range_adaptor_closure<TrimLeading>
        {
            constexpr StrView operator()( StrView Input ) const
            {
                if( Input.starts_with( Pattern ) ) Input.remove_prefix( Pattern.length() );
                return Input;
            }
        };

        struct TrimTrailing : StrViewPattern, RNG::range_adaptor_closure<TrimTrailing>
        {
            constexpr StrView operator()( StrView Input ) const
            {
                if( Input.ends_with( Pattern ) ) Input.remove_suffix( Pattern.length() );
                return Input;
            }
        };

        struct Trim : StrViewPattern, RNG::range_adaptor_closure<Trim>
        {
            constexpr StrView operator()( StrView Input ) const
            {
                while( Input.starts_with( Pattern ) ) Input.remove_prefix( Pattern.length() );
                while( Input.ends_with( Pattern ) ) Input.remove_suffix( Pattern.length() );
                return Input;
            }
        };

        struct Search : StrViewPattern, RNG::range_adaptor_closure<Search>
        {
            constexpr auto operator()( StrView Input ) const -> StrView { return StrView{ RNG::search( Input, Pattern ) }; }
            constexpr StrView In( StrView Input ) const { return operator()( Input ); }
        };

        struct Before : StrViewPattern, RNG::range_adaptor_closure<Before>
        {
            constexpr StrView operator()( StrView Input ) const
            {
                auto [InputBegin, InputEnd] = Input | Boundary;
                auto [MatchBegin, MatchEnd] = Input | Search( Pattern ) | Boundary;
                if( MatchBegin == InputEnd ) return { InputEnd, InputEnd };
                return { InputBegin, MatchBegin };
            }
        };

        struct After : StrViewPattern, RNG::range_adaptor_closure<After>
        {
            constexpr StrView operator()( StrView Input ) const { return { Input | Search( Pattern ) | End, Input | End }; }
        };

        struct Between : RNG::range_adaptor_closure<Between>
        {
            StrViewPattern Left, Right;
            constexpr Between( StrViewPattern Left, StrViewPattern Right ) : Left{ Left }, Right{ Right } {}
            constexpr Between( StrViewPattern Same ) : Between( Same, Same ) {}
            constexpr StrView operator()( StrView Input ) const { return Input | After( Left ) | Before( Right ); }
        };

        struct Count : StrViewPattern, RNG::range_adaptor_closure<Count>
        {
            constexpr std::size_t operator()( StrView Input ) const
            {
                if( Input.empty() ) return 0;
                if( Pattern.empty() ) return Input.length();
                auto OverShoot = Input.ends_with( Pattern ) ? 0 : 1;
                auto Counter = 0uz;
                while( ! Input.empty() )
                {
                    Input = Input | After( Pattern );
                    ++Counter;
                }
                return Counter - OverShoot;
            }
            constexpr std::size_t In( StrView Input ) const { return operator()( Input ); }
        };

        struct SplitOnceBy : StrViewPattern, RNG::range_adaptor_closure<SplitOnceBy>
        {
            using Result = std::array<StrView, 2>;
            constexpr auto operator()( StrView Input ) const -> Result
            {
                if( Input.empty() ) return { Input, Input };
                if( Pattern.empty() ) return { Input.substr( 0, 1 ), Input.substr( 1 ) };
                auto Match = Search( Pattern ).In( Input );
                return { StrView{ Input.begin(), Match.begin() },  //
                         StrView{ Match.end(), Input.end() } };
            }
        };

        struct SplitBy : StrViewPattern, RNG::range_adaptor_closure<SplitBy>
        {
            struct View : RNG::view_interface<View>
            {
                using SplitterType = SplitOnceBy;
                SplitterType::Result ProgressionFrame;
                SplitterType Splitter;
                constexpr View( SplitterType::Result SourceFrame, SplitterType Splitter ) : ProgressionFrame{ SourceFrame }, Splitter{ Splitter } {}
                constexpr View( StrView SourceStrView, SplitterType Splitter ) : View( SourceStrView | Splitter, Splitter ) {}

                struct Iterator
                {
                    using value_type = StrView;
                    using difference_type = StrView::difference_type;
                    SplitterType::Result ProgressionFrame;
                    SplitterType Splitter;
                    bool ReachEnd{ false };
                    constexpr auto operator*() const { return std::get<0>( ProgressionFrame ); }
                    constexpr auto& operator++()
                    {
                        if( std::get<0>( ProgressionFrame ).end() ==  //
                            std::get<1>( ProgressionFrame ).end() )
                            ReachEnd = true;

                        ProgressionFrame = std::get<1>( ProgressionFrame ) | Splitter;
                        return *this;
                    }
                    constexpr auto operator++( int )
                    {
                        auto OldIter = *this;
                        ++*this;
                        return OldIter;
                    }
                    constexpr auto operator==( const Iterator& Other ) const -> bool = default;
                };

                constexpr auto begin() const { return Iterator{ ProgressionFrame, Splitter }; }
                constexpr auto end() const
                {
                    auto SourceEnd = std::get<1>( ProgressionFrame ).end();
                    auto StrViewEnd = StrView{ SourceEnd, SourceEnd };
                    return Iterator{ { StrViewEnd, StrViewEnd }, Splitter, true };
                }
            };

            constexpr auto operator()( StrView Input ) const { return View{ Input, View::SplitterType{ *this } }; }
        };

        struct Split
        {
            StrView Input;
            constexpr auto By( StrViewPattern Pattern ) const { return Input | SplitBy( Pattern ); }
            constexpr auto OnceBy( StrViewPattern Pattern ) const { return Input | SplitOnceBy( Pattern ); }
        };

        template<typename NumericType, int BASE>  //
        struct ConvertToRA : RNG::range_adaptor_closure<ConvertToRA<NumericType, BASE>>
        {
            constexpr static auto operator()( StrView Input ) -> std::optional<NumericType>
            {
                Input = Input | TrimSpace | Trim( '+' ) | TrimSpace;
                NumericType Result;
                if constexpr( std::integral<NumericType> )
                {
                    if( std::from_chars( Input.data(), Input.data() + Input.size(), Result, BASE ) ) return Result;
                }
                else  // floating point
                {
                    if( std::from_chars( Input.data(), Input.data() + Input.size(), Result ) ) return Result;
                }
                return std::nullopt;
            }
        };

        template<typename NumericType, int BASE = 10>  //
        constexpr auto ConvertTo = ConvertToRA<NumericType, BASE>{};

        template<typename NumericType>  //
        struct FallBack : RNG::range_adaptor_closure<FallBack<NumericType>>
        {
            NumericType N;
            constexpr FallBack( NumericType N ) : N{ N } {}
            template<typename OtherNumericType>  //
            constexpr auto operator()( const std::optional<OtherNumericType>& Input ) const -> OtherNumericType
            {
                return Input.value_or( N );
            }
        };

        [[maybe_unused]] constexpr struct RestoreSpaceCharRA : RNG::range_adaptor_closure<RestoreSpaceCharRA>
        {
            constexpr static auto operator()( const auto& Input )
            {
                auto Result = static_cast<std::string>( Input );
                RNG::for_each( Result, []( char& c ) {
                    if( c == '+' ) c = ' ';
                } );
                return Result;
            };
        } RestoreSpaceChar;

        struct SplitAt : RNG::range_adaptor_closure<SplitAt>
        {
            std::size_t N;

            constexpr SplitAt( std::size_t N ) : N{ N } {}

            constexpr auto operator()( StrView Input ) const -> std::array<StrView, 2>
            {
                if( N >= Input.length() )
                    return { Input,  //
                             Input | CollapseToEnd };
                else
                    return { Input.substr( 0, N ),  //
                             Input.substr( N, Input.length() - N ) };
            }

            constexpr auto operator()( auto&& BaseRange ) const
            {
                return std::tuple{ BaseRange | VIEW::take( N ),  //
                                   BaseRange | VIEW::drop( N ) };
            }
        };

        template<typename T>
        struct ToRA : RNG::range_adaptor_closure<ToRA<T>>
        {
            constexpr static auto operator()( const auto& Input ) { return T{ RNG::begin( Input ), RNG::end( Input ) }; }
        };

        template<typename T>
        [[maybe_unused]] constexpr auto To = ToRA<T>{};

    }  // namespace RangeAdaptor

    [[nodiscard]] auto HexToChar( StrView HexString ) noexcept -> char;
    [[nodiscard]] auto DecodeURLFragment( StrView Fragment ) -> std::string;

};  // namespace ParseUtil
using ParseUtil::ConvertTo;

namespace HTTP
{
    enum class StatusCode : unsigned short {
        InternalUse_HeaderAlreadySent = 0,
        OK = 200,
        Created = 201,
        Accepted = 202,
        NoContent = 204,
        BadRequest = 400,
        Unauthorized = 401,
        Forbidden = 403,
        NotFound = 404,
        MethodNotAllowed = 405,
        UnsupportedMediaType = 415,
        UnprocessableEntity = 422,
        InternalServerError = 500,
        NotImplemented = 501,
        ServiceUnavailable = 503,
    };

    struct RequestMethod
    {
        enum class EnumValue : unsigned short { INVALID, GET, HEAD, POST, PUT, DELETE, CONNECT, OPTIONS, TRACE, PATCH };
        EnumValue Verb;

#define RETURN_IF( N ) \
    if( VerbName == #N ) return N
        static constexpr auto FromStringView( std::string_view VerbName )
        {
            using enum EnumValue;
            RETURN_IF( GET );
            RETURN_IF( PUT );
            RETURN_IF( POST );
            RETURN_IF( HEAD );
            RETURN_IF( PATCH );
            RETURN_IF( TRACE );
            RETURN_IF( DELETE );
            RETURN_IF( OPTIONS );
            RETURN_IF( CONNECT );
            RETURN_IF( INVALID );
            return INVALID;
        }
#undef RETURN_IF

#define RETURN_CASE( N ) \
    case N : return #N
        static constexpr auto ToStringView( EnumValue Verb ) -> std::string_view
        {
            switch( Verb )
            {
                using enum EnumValue;
                RETURN_CASE( GET );
                RETURN_CASE( PUT );
                RETURN_CASE( POST );
                RETURN_CASE( HEAD );
                RETURN_CASE( PATCH );
                RETURN_CASE( TRACE );
                RETURN_CASE( DELETE );
                RETURN_CASE( CONNECT );
                RETURN_CASE( OPTIONS );
                RETURN_CASE( INVALID );
                default : return "INVALID";
            }
            std::unreachable();
        }
#undef RETURN_CASE

        constexpr RequestMethod() = default;
        constexpr RequestMethod( const RequestMethod& ) = default;
        constexpr RequestMethod( EnumValue OtherVerb ) : Verb{ OtherVerb } {}
        constexpr RequestMethod( std::string_view VerbName ) : RequestMethod( FromStringView( VerbName ) ) {}

        using FormatAs = std::string_view;
        constexpr operator std::string_view() const { return ToStringView( Verb ); }
        constexpr auto EnumLiteral() const { return ToStringView( Verb ); }
        constexpr operator EnumValue() const { return Verb; }
    };

    // for better auto completion
    namespace Request
    {
        inline namespace Method
        {
            constexpr RequestMethod INVALID{ RequestMethod::EnumValue::INVALID };
            constexpr RequestMethod GET{ RequestMethod::EnumValue::GET };
            constexpr RequestMethod HEAD{ RequestMethod::EnumValue::HEAD };
            constexpr RequestMethod POST{ RequestMethod::EnumValue::POST };
            constexpr RequestMethod PUT{ RequestMethod::EnumValue::PUT };
            constexpr RequestMethod DELETE{ RequestMethod::EnumValue::DELETE };
            constexpr RequestMethod CONNECT{ RequestMethod::EnumValue::CONNECT };
            constexpr RequestMethod OPTIONS{ RequestMethod::EnumValue::OPTIONS };
            constexpr RequestMethod TRACE{ RequestMethod::EnumValue::TRACE };
            constexpr RequestMethod PATCH{ RequestMethod::EnumValue::PATCH };
        }  // namespace Method
    };  // namespace Request

    struct ContentType
    {
        enum class EnumValue : unsigned short {
            TEXT_PLAIN,
            TEXT_HTML,
            TEXT_XML,
            TEXT_CSV,
            TEXT_CSS,
            TEXT_EVENT_STREAM,
            APPLICATION_JSON,
            APPLICATION_X_WWW_FORM_URLENCODED,
            APPLICATION_OCTET_STREAM,
            MULTIPART_FORM_DATA,
            MULTIPART_BYTERANGES,
            UNKNOWN_MIME_TYPE,
        };
        EnumValue Type;

        static constexpr auto FromStringView( std::string_view TypeName )
        {
            using enum EnumValue;
            if( TypeName.starts_with( "text/plain" ) ) return TEXT_PLAIN;
            if( TypeName.starts_with( "text/html" ) ) return TEXT_HTML;
            if( TypeName.starts_with( "text/xml" ) ) return TEXT_XML;
            if( TypeName.starts_with( "text/csv" ) ) return TEXT_CSV;
            if( TypeName.starts_with( "text/css" ) ) return TEXT_CSS;
            if( TypeName.starts_with( "text/event-stream" ) ) return TEXT_EVENT_STREAM;
            if( TypeName.starts_with( "application/json" ) ) return APPLICATION_JSON;
            if( TypeName.starts_with( "application/x-www-form-urlencoded" ) ) return APPLICATION_X_WWW_FORM_URLENCODED;
            if( TypeName.starts_with( "application/octet-stream" ) ) return APPLICATION_OCTET_STREAM;
            if( TypeName.starts_with( "multipart/form-data" ) ) return MULTIPART_FORM_DATA;
            if( TypeName.starts_with( "multipart/byteranges" ) ) return MULTIPART_BYTERANGES;
            return UNKNOWN_MIME_TYPE;
        }

        static constexpr auto ToStringView( EnumValue Type ) -> std::string_view
        {
            switch( Type )
            {
                using enum EnumValue;
                case TEXT_PLAIN :                        return "text/plain";
                case TEXT_HTML :                         return "text/html";
                case TEXT_XML :                          return "text/xml";
                case TEXT_CSV :                          return "text/csv";
                case TEXT_CSS :                          return "text/css";
                case TEXT_EVENT_STREAM :                 return "text/event-stream";
                case APPLICATION_JSON :                  return "application/json";
                case APPLICATION_X_WWW_FORM_URLENCODED : return "application/x-www-form-urlencoded";
                case APPLICATION_OCTET_STREAM :          return "application/octet-stream";
                case MULTIPART_FORM_DATA :               return "multipart/form-data";
                case MULTIPART_BYTERANGES :              return "multipart/byteranges";
                // default :
                case UNKNOWN_MIME_TYPE : return ToStringView( TEXT_HTML );
            }
            return "";
        }

        constexpr ContentType() = default;
        constexpr ContentType( const ContentType& ) = default;
        constexpr ContentType( EnumValue Other ) : Type{ Other } {}
        constexpr ContentType( std::string_view TypeName ) : ContentType{ FromStringView( TypeName ) } {}

        using FormatAs = std::string_view;
        constexpr operator std::string_view() const { return ToStringView( Type ); }
        constexpr auto EnumLiteral() const { return ToStringView( Type ); }
        constexpr operator EnumValue() const { return Type; }
    };

    // for better auto completion
    namespace Content
    {
        inline namespace Type
        {
            namespace Text
            {
                constexpr ContentType Plain = ContentType::EnumValue::TEXT_PLAIN;
                constexpr ContentType HTML = ContentType::EnumValue::TEXT_HTML;
                constexpr ContentType XML = ContentType::EnumValue::TEXT_XML;
                constexpr ContentType CSV = ContentType::EnumValue::TEXT_CSV;
                constexpr ContentType CSS = ContentType::EnumValue::TEXT_CSS;
                constexpr ContentType EventStream = ContentType::EnumValue::TEXT_EVENT_STREAM;
            }  // namespace Text

            namespace Application
            {
                constexpr ContentType Json = ContentType::EnumValue::APPLICATION_JSON;
                constexpr ContentType FormURLEncoded = ContentType::EnumValue::APPLICATION_X_WWW_FORM_URLENCODED;
                constexpr ContentType OctetStream = ContentType::EnumValue::APPLICATION_OCTET_STREAM;
            };

            namespace MultiPart
            {
                constexpr ContentType FormData = ContentType::EnumValue::MULTIPART_FORM_DATA;
                constexpr ContentType ByteRanges = ContentType::EnumValue::MULTIPART_BYTERANGES;
            };
        }  // namespace Type
    }  // namespace Content
}  // namespace HTTP

namespace EasyFCGI
{
    namespace FS = std::filesystem;
    namespace RNG = std::ranges;
    namespace VIEW = std::views;
    using Json = glz::json_t;
    using StrView = std::string_view;
    using Clock = std::chrono::system_clock;

    namespace Config
    {
        extern int DefaultBackLogNumber;  // evetually be capped by /proc/sys/net/core/somaxconncat,ie.4096
        extern bool RunAsDaemon;
        extern FS::path CWD;
        extern FS::path LogFilePath;
        extern FS::path PidFilePath;
        extern FS::path SokcetPath;
        auto LaunchOptionContains( StrView ) -> bool;                 // only target switch option format : -abc / --foo
        auto LaunchOptionValue( StrView ) -> std::optional<StrView>;  // only target option with arg : --foo=bar / --foo baz
    }  // namespace Config

    auto DumpJson( const EasyFCGI::Json& ) -> std::string;

    // extern std::stop_source TerminationSource;
    extern std::stop_token TerminationToken;
    auto TerminationRequested() -> bool;

    // Return:
    // [ true ]  if successfully slept for Duration;
    // [ false ] if TerminationToken activated
    auto SleepFor( Clock::duration Duration ) -> bool;

    using SocketFileDescriptor = decltype( FCGX_OpenSocket( {}, {} ) );
    // using ConnectionFileDescriptor = decltype( ::accept( {}, {}, {} ) );

    struct Response
    {
        HTTP::StatusCode StatusCode = HTTP::StatusCode::OK;
        HTTP::ContentType ContentType = HTTP::Content::Text::HTML;
        std::flat_map<std::string, std::string> Header{};
        std::flat_map<std::string, std::string> Cookie{};
        std::string Body{};

        Response& Set( HTTP::StatusCode ) &;
        Response& Set( HTTP::ContentType ) &;
        Response& SetHeader( const std::string&, std::string ) &;
        Response& SetCookie( const std::string&, std::string ) &;

        Response& SetBody( std::string ) &;
        Response& Append( StrView ) &;

        Response& SetBody( const char* ) &;
        Response& Append( const char* ) &;

        Response& SetBody( const Json& ) &;
        Response& Append( const Json& ) &;

        template<typename T>
        Response& operator=( T&& Value ) &
        {
            return SetBody( std::forward<T>( Value ) );
        };
        template<typename T>
        Response& operator+=( T&& Value ) &
        {
            return Append( std::forward<T>( Value ) );
        };

        Response& Reset() &;
    };

    struct Request
    {
        struct Query
        {
            EasyFCGI::Json Json;
            auto contains( StrView ) const -> bool;
            auto CountRepeated( StrView ) const -> decltype( Json.size() );
            static auto StringDump( const EasyFCGI::Json& ) -> std::string;
            auto operator[]( StrView, std::size_t = 0 ) const -> std::string;
            auto GetOptional( StrView, std::size_t = 0 ) const -> std::optional<std::string>;
        };

        struct Cookie
        {
            FCGX_ParamArray EnvPtr;
            auto operator[]( StrView ) const -> StrView;
        };

        struct Header
        {
            FCGX_ParamArray EnvPtr;
            auto operator[]( StrView ) const -> StrView;
            auto contains( StrView ) const -> bool;
        };

        struct Files
        {
            enum class OverWriteOptions : unsigned char { Abort, OverWrite, RenameOldFile, RenameNewFile };

            struct FileView
            {
                using enum OverWriteOptions;
                StrView FileName{};
                StrView ContentType{};
                StrView ContentBody{};

                auto SaveAs( const FS::path&, const OverWriteOptions = Abort ) const -> std::optional<FS::path>;
            };

            std::flat_map<StrView, std::vector<FileView>> Storage;
            auto operator[]( StrView, std::size_t = 0 ) const -> FileView;
        };

        struct Response Response;
        struct Files Files;
        std::string Payload;
        struct Query Query;
        struct Header Header;
        struct Cookie Cookie;
        std::unique_ptr<FCGX_Request> FCGX_Request_Ptr;
        HTTP::RequestMethod Method;
        HTTP::ContentType ContentType;

        auto GetParam( StrView ParamName ) const -> StrView;  // Read FCGI envirnoment variables set up by upstream server
        auto AllHeaderEntries() const -> std::vector<StrView>;
        auto Accept() -> int;
        auto Parse() -> int;
        explicit operator bool() const;

        Request() = default;
        Request( Request&& Other ) = default;
        Request( const Request& ) = delete;

        Request( SocketFileDescriptor );

        Request& operator=( Request&& Other ) &;

        auto empty() const -> bool;

        auto operator[]( StrView, std::size_t = 0 ) const -> decltype( Query[{}, {}] );

        auto Send( StrView ) const -> void;
        auto SendLine( StrView = {} ) const -> void;

        // template<typename... Args>
        // requires( sizeof...( Args ) > 0 )
        // auto Send( const std::format_string<Args...>&, Args&&... ) const;

        // template<typename... Args>
        // requires( sizeof...( Args ) > 0 )
        // auto SendLine( const std::format_string<Args...>&, Args&&... ) const;

        auto FlushHeader() -> HTTP::StatusCode;
        auto FlushResponse() -> decltype( FCGX_FFlush( {} ) );
        auto EarlyFinish() -> void;
        auto SSE_Start() -> void;
        auto SSE_Error() const -> decltype( FCGX_GetError( {} ) );
        auto SSE_Send( std::convertible_to<StrView> auto&&... Content ) const
        {
            ( Send( Content ), ... );
            SendLine();
            SendLine();
            return FCGX_FFlush( FCGX_Request_Ptr->out );
        };

        auto Dump() const -> std::string;

        // virtual
        ~Request();
    };

    // auto UnixSocketName( SocketFileDescriptor FD ) -> FS::path;
    struct Server
    {
        struct RequestQueue
        {
            SocketFileDescriptor ListenSocket;
            Request PendingRequest;
            RequestQueue() = delete;
            RequestQueue( const RequestQueue& ) = delete;
            RequestQueue( SocketFileDescriptor );
            auto PreparePendingRequest() -> bool;
            auto RetrievePendingRequest() -> Request;
            auto DropPendingRequest() -> void;
            auto empty() const -> bool;

            // possible usage:
            // while ( auto R = Server.RequestQueue.NextRequest() ){ ... }
            auto NextRequest() -> Request;

            struct Sentinel
            {};

            struct Iterator
            {
                using value_type = Request;
                using difference_type = std::ptrdiff_t;
                RequestQueue* AttachedQueuePtr;
                auto operator++() & -> Iterator&;
                auto operator++( int ) -> Iterator;
                auto operator*() const -> Request;
                auto operator==( Sentinel ) const -> bool;
            };

            auto begin() -> Iterator;
            auto end() const -> Sentinel;
        } RequestQueue;

        Server( SocketFileDescriptor );
        Server();
        Server( const FS::path& );
    };
}  // namespace EasyFCGI

// enable formatter for HTTP constant objects
template<typename T>
requires requires { typename T::FormatAs; }
struct std::formatter<T> : std::formatter<typename T::FormatAs>
{
    auto format( const T& Value, std::format_context& ctx ) const  //
    {
        return std::formatter<typename T::FormatAs>::format( Value, ctx );
    }
};

//explicit template instantiation
extern template struct std::formatter<HTTP::RequestMethod>;
extern template struct std::formatter<HTTP::ContentType>;
extern template struct ParseUtil::ConvertToRA<int, 10>;
extern template struct ParseUtil::FallBack<int>;
extern template const ParseUtil::ConvertToRA<int, 10> ParseUtil::ConvertTo<int>;

#endif