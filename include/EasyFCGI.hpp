#ifndef _EASY_FCGI_HPP
#define _EASY_FCGI_HPP
#include <fcgiapp.h>
#include <csignal>
#include <memory>
#include <concepts>
#include <utility>
#include <vector>
#include <ranges>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <chrono>
#include <print>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <thread>
#include <stop_token>
#include "json.hpp"

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
        char Data[N];
        constexpr FixedString( const char ( &Src )[N] ) noexcept { std::copy_n( Src, N, Data ); }
        constexpr operator StrView() const noexcept { return { Data, N - 1 }; }
    };

    template<FixedString FSTR>
    constexpr auto operator""_FMT() noexcept
    {
        return []( auto&&... args ) { return std::format( FSTR, std::forward<decltype( args )>( args )... ); };
    }

    struct StrViewPattern
    {
        constexpr static auto ASCII = [] {
            auto ASCII = std::array<char, 127>{};
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
            constexpr auto operator()( StrView Input ) const -> StrView
            {
                if consteval { return StrView{ RNG::search( Input, Pattern ) }; }
                else
                {
                    // auto Match = std::search( Input.begin(), Input.end(),  //
                    //                           std::boyer_moore_horspool_searcher( Pattern.begin(), Pattern.end() ) );
                    // return { Match, Match == Input.end() ? Match : std::next( Match, Pattern.length() ) };
                    auto [MatchBegin, MatchEnd] = std::boyer_moore_horspool_searcher( Pattern.begin(), Pattern.end() )( Input.begin(), Input.end() );
                    return { MatchBegin, MatchEnd };
                }
            }
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
            constexpr StrView operator()( StrView Input ) const
            {
                // auto [InputBegin, InputEnd] = Input | Boundary;
                // auto [MatchBegin, MatchEnd] = Input | Search( Pattern ) | Boundary;
                return { Input | Search( Pattern ) | End, Input | End };
            }
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

        struct SplitBy_OLD : StrViewPattern, RNG::range_adaptor_closure<SplitBy_OLD>
        {
            constexpr auto operator()( StrView Input ) const
            {
                auto Result = std::vector<StrView>();
                Result.reserve( Count( Pattern ).In( Input ) + 1 );

                auto EndsWithPattern = Input.ends_with( Pattern );
                if consteval
                {
                    do std::forward_as_tuple( std::back_inserter( Result ), Input ) = Input | SplitOnceBy( Pattern );
                    while( ! Input.empty() );
                }
                else
                {
                    auto BMH = std::boyer_moore_horspool_searcher( Pattern.begin(), Pattern.end() );
                    do {
                        auto [MatchBegin, MatchEnd] = BMH( Input.begin(), Input.end() );
                        Result.emplace_back( Input.begin(), MatchBegin );
                        Input = { MatchEnd, Input.end() };
                    } while( ! Input.empty() );
                }

                if( EndsWithPattern ) Result.push_back( Input );

                return Result;
            }
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
            constexpr auto operator()( const std::optional<OtherNumericType>& Input ) const
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

    }  // namespace RangeAdaptor

    [[nodiscard]]
    static auto HexToChar( StrView HexString ) noexcept
    {
        return std::bit_cast<char>( HexString | ConvertTo<unsigned char, 16> | FallBack( '?' ) );
    }

    [[nodiscard]]
    static auto DecodeURLFragment( StrView Fragment )
    {
        constexpr auto EncodeDigitWidth = 2;
        auto Result = std::string{};
        auto [FirstPart, OtherParts] = Fragment | SplitBy( '%' ) | SplitAt( 1 );
        for( auto LeadingText : FirstPart ) Result += LeadingText | RestoreSpaceChar;
        for( auto Segment : OtherParts )
        {
            auto [Encoded, Unencoded] = Segment | SplitAt( EncodeDigitWidth );
            Result += Encoded.length() >= EncodeDigitWidth ? HexToChar( Encoded ) : '?';
            Result += Unencoded | RestoreSpaceChar;
        }
        return Result;
    }
};  // namespace ParseUtil

namespace HTTP
{
    namespace PU = ParseUtil;

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
            if( TypeName == "text/plain" ) return TEXT_PLAIN;
            if( TypeName == "text/html" ) return TEXT_HTML;
            if( TypeName == "text/xml" ) return TEXT_XML;
            if( TypeName == "text/csv" ) return TEXT_CSV;
            if( TypeName == "text/css" ) return TEXT_CSS;
            if( TypeName == "text/event-stream" ) return TEXT_EVENT_STREAM;
            if( TypeName == "application/json" ) return APPLICATION_JSON;
            if( TypeName == "application/x-www-form-urlencoded" ) return APPLICATION_X_WWW_FORM_URLENCODED;
            if( TypeName == "application/octet-stream" ) return APPLICATION_OCTET_STREAM;
            if( TypeName == "multipart/form-data" ) return MULTIPART_FORM_DATA;
            if( TypeName == "multipart/byteranges" ) return MULTIPART_BYTERANGES;
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
                case UNKNOWN_MIME_TYPE :                 return ToStringView( TEXT_HTML );
            }
            return "";
        }

        constexpr ContentType() = default;
        constexpr ContentType( const ContentType& ) = default;
        constexpr ContentType( EnumValue Other ) : Type{ Other } {}
        constexpr ContentType( std::string_view TypeName ) : Type{ FromStringView( TypeName | PU::SplitBy( ';' ) | PU::Front | PU::TrimSpace ) } {}

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
    using Json = nlohmann::json;
    using StrView = std::string_view;
    using ParseUtil::operator""_FMT;

    inline namespace Concept
    {
        template<typename T>
        concept DumpingString = requires( T&& t ) {
            { t.dump() } -> std::same_as<std::string>;
        };
    }

    struct Config
    {
        // evetually capped by /proc/sys/net/core/somaxconncat,eg.4096
        constexpr static auto DefaultBackLogNumber = 128;

        // static auto SocketDirectoryPrefix = StrView{"/tmp"};
        constexpr static auto SocketDirectoryPrefix = StrView{ "/dev/shm/" };
        constexpr static auto SocketExtensionSuffix = StrView{ ".sock" };

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

            return VIEW::split( CommandLineBuffer, '\0' )  //
                   | VIEW::transform( RNG::data )          //
                   | RNG::to<std::vector>();
        }();

        inline static auto DefaultSocketPath = FS::path( Config::SocketDirectoryPrefix ) /  //
                                               FS::path( CommandLine[0] )
                                                   .filename()  //
                                                   .replace_extension( Config::SocketExtensionSuffix );

        inline static auto OptionSocketPath = [] -> std::optional<FS::path> {
            for( auto&& [Option, OptionArg] : CommandLine | VIEW::pairwise )
                if( Option == StrView{ "-s" } )  //
                    return OptionArg;
            return {};
        }();

        inline static std::function<void( int )> ClientSpaceSignalHandler{};
    };

    struct ScopedTimer
    {
        constexpr static auto Silent = true;
        inline static std::atomic<std::size_t> LatestTimerID = 0;

        using Clock = std::chrono::steady_clock;
        Clock::time_point StartTime;
        std::string Marker;
        std::size_t ID;
        ScopedTimer( std::convertible_to<std::string> auto&& Marker = "" )  //
            : StartTime{ Silent ? Clock::time_point{} : Clock::now() },
              Marker{ Silent ? "" : Marker },
              ID{ Silent ? 0 : ++LatestTimerID }
        {
            if constexpr( Silent ) { return; }
            else { std::println( "[ ScopedTimer {:2} ] | <{}> | Start ", ID, Marker ); }
        }
        ~ScopedTimer()
        {
            if constexpr( Silent ) { return; }
            else { std::println( "[ ScopedTimer {:2} ] | <{}> | End | {:%M min %S sec}  Elapsed ", ID, Marker, Clock::now() - StartTime ); }
        }
    };

    // static auto TerminationSignal = std::atomic<bool>{ false };

    static auto TerminationSource = std::stop_source{};
    static auto TerminationToken = TerminationSource.get_token();
    // static struct TerminationSignal_Impl : std::stop_token
    // {
    //     operator bool() const { return stop_requested(); }
    //     bool operator()() const { return stop_requested(); }
    //     bool load() const { return stop_requested(); }
    //     auto store( bool Stop ) const
    //     {
    //         if( Stop ) TerminationSource.request_stop();
    //     }
    // } TerminationToken{ TerminationSource.get_token() };

    extern "C" void OS_LibShutdown();  // for omitting #include <fcgios.h>
    static auto ServerInitialization = [] {
        static auto ServerInitializationComplete = false;
        if( std::exchange( ServerInitializationComplete, true ) ) return;

        if( auto ErrorCode = FCGX_Init(); ErrorCode == 0 )
        {
            std::println( "[ OK ]  ServerInitialization : FCGX_Init" );
            std::atexit( OS_LibShutdown );
            std::atexit( [] {
                std::println(
                    "Porgram exiting from standard exit pathway.\n"
                    "Termination Signal : {}",
                    TerminationToken.stop_requested() );
            } );

            struct sigaction SignalAction;
            sigemptyset( &SignalAction.sa_mask );
            SignalAction.sa_flags = 0;  // disable SA_RESTART
            SignalAction.sa_handler = []( int Signal ) {
                TerminationSource.request_stop();
                FCGX_ShutdownPending();
                std::println( "\nReceiving Signal : {}", Signal );
                if( Config::ClientSpaceSignalHandler ) Config::ClientSpaceSignalHandler( Signal );
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

    using SocketFileDescriptor = decltype( FCGX_OpenSocket( {}, {} ) );
    using ConnectionFileDescriptor = decltype( ::accept( {}, {}, {} ) );

    static auto PollFor( int FD, unsigned int Flags, int Timeout = -1 )
    {
        auto PollFD = pollfd{ .fd = FD, .events = static_cast<short>( Flags ), .revents = 0 };
        return ::poll( &PollFD, 1, Timeout ) > 0 && ( PollFD.revents & Flags ) == Flags;
    }

    struct ReusableFD
    {
        ReusableFD()
        {
            if( ::pipe( PipeFD ) != 0 )
            {
                std::println( "Fail to create PendingFD Pipe" );
                std::exit( 0 );
            }
        }
        ~ReusableFD()
        {
            ::close( PipeFD[0] );
            ::close( PipeFD[1] );
        }
        mutable std::mutex PipeLock;
        int PipeFD[2];

        auto Store( ConnectionFileDescriptor FD ) const
        {
            auto _ = std::lock_guard{ PipeLock };
            return ::write( PipeFD[1], &FD, sizeof( FD ) );
        }
        auto Load() const -> std::optional<ConnectionFileDescriptor>
        {
            auto _ = std::lock_guard{ PipeLock };
            if( PollFor( PipeFD[0], POLLIN, 0 ) )
            {
                auto FD = ConnectionFileDescriptor{};
                if( ::read( PipeFD[0], &FD, sizeof( FD ) ) >= 0 ) return FD;
            }
            return std::nullopt;
        }
        auto empty() const { return ! PollFor( PipeFD[0], POLLIN, 0 ); }
    };

    struct Response
    {
        HTTP::StatusCode StatusCode{ HTTP::StatusCode::OK };
        HTTP::ContentType ContentType{ HTTP::Content::Text::Plain };
        std::map<std::string, std::string> Header;
        std::map<std::string, std::string> Cookie;
        std::string Body;

        [[maybe_unused]] decltype( auto ) Set( HTTP::StatusCode NewValue ) { return StatusCode = NewValue, *this; }
        [[maybe_unused]] decltype( auto ) Set( HTTP::ContentType NewValue ) { return ContentType = NewValue, *this; }
        [[maybe_unused]] decltype( auto ) SetHeader( const std::string& Key, auto&& Value )
        {
            Header[Key] = std::forward<decltype( Value )>( Value );
            return *this;
        }
        [[maybe_unused]] decltype( auto ) SetCookie( const std::string& Key, auto&& Value )
        {
            Cookie[Key] = std::forward<decltype( Value )>( Value );
            return *this;
        }
        [[maybe_unused]] decltype( auto ) Reset()
        {
            Set( HTTP::StatusCode::OK );
            Set( HTTP::Content::Text::Plain );
            Header.clear();
            Cookie.clear();
            Body.clear();
            return *this;
        }

        template<typename T>  //requires( ! std::same_as<std::remove_cvref_t<T>, Response> )
        [[maybe_unused]] decltype( auto ) operator=( T&& NewContent )
        {
            if constexpr( DumpingString<T> ) { Body = std::forward<T>( NewContent ).dump(); }
            else { Body = std::forward<T>( NewContent ); }
            return *this;
        }

        template<typename T>  //
        [[maybe_unused]] decltype( auto ) Append( T&& NewContent )
        {
            if( Body.empty() ) return operator=( std::forward<T>( NewContent ) );
            if constexpr( DumpingString<T> ) { Body.append( NewContent.dump() ); }
            else { Body.append( std::forward<T>( NewContent ) ); }
            return *this;
        }

        template<typename T>                                            //
        [[maybe_unused]] decltype( auto ) operator+=( T&& NewContent )  //
        {
            return Append( std::forward<T>( NewContent ) );
        }
    };

    struct Request
    {
        struct Query
        {
            EasyFCGI::Json Json;
            auto contains( StrView Key ) const { return Json.contains( Key ); }
            auto CountRepeated( StrView Key ) const -> decltype( Json.size() )
            {
                if( ! Json.contains( Key ) ) return 0;
                return Json[Key].size();
            }
            auto operator[]( StrView Key, std::size_t Index = 0 ) const -> std::string
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
        };

        struct Cookie
        {
            FCGX_ParamArray EnvPtr;
            auto operator[]( StrView Key ) const -> StrView
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
        };

        struct Header
        {
            FCGX_ParamArray EnvPtr;
            auto operator[]( StrView Key ) const -> StrView
            {
                if( EnvPtr == nullptr ) return {};
                auto FullHeaderKey = "HTTP_{}"_FMT( Key );
                for( auto& C : FullHeaderKey )
                {
                    if( C == '-' ) C = '_';
                    C = std::toupper( C );
                }
                auto Result = FCGX_GetParam( FullHeaderKey.c_str(), EnvPtr );
                if( Result == nullptr ) return {};
                return Result;
            }
        };

        struct Files
        {
            enum class OverWriteOptions : unsigned char { Abort, OverWrite, RenameOldFile, RenameNewFile };

            struct FileView
            {
                using enum OverWriteOptions;
                StrView FileName;
                StrView ContentType;
                StrView ContentBody;

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

                auto SaveAs( const FS::path& Path, const OverWriteOptions OverWriteOption = OverWriteOptions::Abort ) const -> std::optional<FS::path>
                {
                    auto ParentDir = Path.parent_path();
                    if( ! FS::exists( ParentDir ) ) FS::create_directories( ParentDir );

                    auto ResultPath = Path;
                    if( FS::exists( ResultPath ) )  //
                        switch( OverWriteOption )
                        {
                            using enum OverWriteOptions;
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
            };

            std::map<StrView, std::vector<FileView>> Storage;
            auto operator[]( StrView Key, std::size_t Index = 0 ) const -> FileView
            {
                if( ! Storage.contains( Key ) ) return {};
                const auto& Slot = Storage.at( Key );
                if( Index < Slot.size() ) return Slot[Index];
                return {};
            }
        };

        struct Response Response;  // elaborated-type-specifier, silencing -Wchanges-meaning
        struct Files Files;
        std::string Payload;
        struct Query Query;
        struct Header Header;
        struct Cookie Cookie;
        std::unique_ptr<FCGX_Request> FCGX_Request_Ptr;
        HTTP::RequestMethod Method;
        HTTP::ContentType ContentType;
        ReusableFD* ReusableFD_Ptr;

        // Read FCGI envirnoment variables set up by upstream server
        auto GetParam( StrView ParamName ) const -> StrView
        {
            if( auto Result = FCGX_GetParam( std::data( ParamName ), FCGX_Request_Ptr->envp ) ) return Result;
            return {};
        }

        auto AllHeaderEntries() const
        {
            auto Result = std::vector<std::string_view>{};
            Result.reserve( 100 );
            for( auto EnvP = FCGX_Request_Ptr->envp; EnvP != nullptr && *EnvP != nullptr; ++EnvP ) { Result.push_back( *EnvP ); }
            return Result;
        }

        auto Parse() -> int
        {
            // if not using nginx, disable persistent connection
            if( GetParam( "SERVER_SOFTWARE" ).contains( "Apache" ) ) FCGX_Request_Ptr->keepConnection = false;
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
                        auto _ = ScopedTimer( "MultipartParseTime" );
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

                            if( ContentType.empty() ) { QueryAppend( Name, Content ); }
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
                            FCGX_PutS(
                                "Status: 400\r\n"
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

        Request() = default;
        Request( const Request& ) = delete;
        Request( Request&& Other ) = default;

        Request( SocketFileDescriptor SocketFD, ConnectionFileDescriptor ConnectionFD, ReusableFD* ReusableFD_Ptr )  //
            : FCGX_Request_Ptr{ std::make_unique_for_overwrite<FCGX_Request>().release() },
              ReusableFD_Ptr{ ReusableFD_Ptr }
        {
            auto Request_Ptr = FCGX_Request_Ptr.get();
            (void)FCGX_InitRequest( Request_Ptr, SocketFD, FCGI_FAIL_ACCEPT_ON_INTR );
            Request_Ptr->ipcFd = ConnectionFD;  // specify ipcFD, enable persistent connection

            if( FCGX_Accept_r( Request_Ptr ) == 0 )
            {
                // FCGX_Request_Ptr ready, setup the rest of request object(parse request)
                if( Parse() == 0 ) { return; }
            }

            // fail to obtain valid request, reset residual request data & allocation
            // custom deleter of FCGX_Request_Ptr will do the cleanup
            FCGX_InitRequest( Request_Ptr, {}, {} );
            FCGX_Request_Ptr.reset();

            if( TerminationToken.stop_requested() ) std::println( "Interrupted FCGX_Accept_r()." );
        }

        static auto AcceptFrom( auto&&... args ) { return Request{ std::forward<decltype( args )>( args )... }; }

        Request& operator=( Request&& Other ) = default;

        auto empty() const { return FCGX_Request_Ptr == nullptr; }

        auto operator[]( StrView Key, std::size_t Index = 0 ) const { return Query[Key, Index]; }

        auto OutputIterator() const
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

        auto Send( StrView Content ) const
        {
            if( Content.empty() || FCGX_Request_Ptr == nullptr ) return;
            FCGX_PutStr( Content.data(), Content.length(), FCGX_Request_Ptr->out );
        }

        auto SendLine( StrView Content = {} ) const
        {
            Send( Content );
            Send( "\r\n" );
        }

        template<typename... Args>
        requires( sizeof...( Args ) > 0 )                                          //
        auto Send( const std::format_string<Args...>& fmt, Args&&... args ) const  //
        {
            if( FCGX_Request_Ptr == nullptr ) return;
            std::format_to( OutputIterator(), fmt, std::forward<Args>( args )... );
        }

        template<typename... Args>
        requires( sizeof...( Args ) > 0 )                                              //
        auto SendLine( const std::format_string<Args...>& fmt, Args&&... args ) const  //
        {
            Send( fmt, std::forward<Args>( args )... );
            SendLine();
        }

        auto FlushHeader()
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

        auto FlushResponse()
        {
            Send( Response.Body );
            Response.Body.clear();
            return FCGX_FFlush( FCGX_Request_Ptr->out );
        }

        auto EarlyFinish() { std::exchange( *this, {} ); }

        auto SSE_Start()
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

        auto SSE_Send( auto&&... Content ) const
        {
            ( Send( Content ), ... );
            SendLine();
            SendLine();
            return FCGX_FFlush( FCGX_Request_Ptr->out );
        };

        auto SSE_Error() const { return FCGX_GetError( FCGX_Request_Ptr->out ); }

        virtual ~Request()
        {
            if( ! FCGX_Request_Ptr ) return;
            // std::println( "ID: [ {:2},{:2} ] Request Complete...", FCGX_Request_Ptr->ipcFd, FCGX_Request_Ptr->requestId );
            if( FlushHeader() != HTTP::StatusCode::NoContent ) FlushResponse();
            FCGX_Finish_r( FCGX_Request_Ptr.get() );
            if( ReusableFD_Ptr && FCGX_Request_Ptr->ipcFd != -1 ) ReusableFD_Ptr->Store( FCGX_Request_Ptr->ipcFd );
        }
    };

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

    struct Server
    {
        inline static auto ServerInitializationExec = ( ServerInitialization(), 0 );

        struct RequestQueue
        {
            RequestQueue() = delete;
            RequestQueue( const RequestQueue& ) = delete;
            RequestQueue( SocketFileDescriptor SourceSocketFD ) : ListenSocket{ SourceSocketFD } {};

            SocketFileDescriptor ListenSocket;

            struct ReusableFD ReusableFD{};

            auto WaitForListenSocket( int Timeout = -1 ) const { return PollFor( ListenSocket, POLLIN, Timeout ); }
            auto ListenSocketActivated() const { return WaitForListenSocket( 0 ); }

            auto NextRequest() { return Request::AcceptFrom( ListenSocket, ReusableFD.Load().value_or( -1 ), &ReusableFD ); }

            struct Sentinel
            {};
            struct Iterator
            {
                RequestQueue& AttachedQueue;
                auto& operator++() &
                {
                    //no-op
                    return *this;
                }
                auto operator*() const { return AttachedQueue.ListenSocketActivated() ? AttachedQueue.NextRequest() : Request{}; }
                bool operator==( Sentinel ) const
                {
                    auto _ = ScopedTimer( "WaitForListenSocket" );
                    return ! AttachedQueue.WaitForListenSocket();
                }
            };

            auto begin() { return Iterator{ *this }; }
            auto end() const { return Sentinel{}; }
        } RequestQueue;

        Server( SocketFileDescriptor ListenSocket ) : RequestQueue{ ListenSocket }
        {
            std::println( "Server file descriptor : {}", static_cast<int>( ListenSocket ) );
            std::println( "Unix Socket Path : {}", UnixSocketName( ListenSocket ).c_str() );
            std::println( "Ready to accept requests..." );
        }

        // Server() : Server{ SocketFileDescriptor{} }
        Server() : Server( Config::OptionSocketPath.value_or( Config::DefaultSocketPath ) ) {}
        Server( const FS::path& SocketPath )
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
            if( FD > 0 ) { FS::permissions( SocketPath, FS::perms::all ); }
        }
    };

    namespace DebugInfo
    {
        typedef struct FCGX_Stream_Data
        {
            unsigned char* buff;
            int bufflen;
            unsigned char* mBuff;
            unsigned char* buffStop;
            int type;
            int eorStop;
            int skip;
            int contentLen;
            int paddingLen;
            int isAnythingWritten;
            int rawWrite;
            FCGX_Request* reqDataPtr;
        } FCGX_Stream_Data;
    }  // namespace DebugInfo

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

#endif
