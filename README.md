> # EasyFCGI
> C++26 Wrapper for libfcgi

---

> # Quick Start Demo
> ```c++
> #include "include/EasyFCGI.hpp"
> 
> int main(){
>   auto Server = EasyFCGI::Server{"path/to/unix/socket.sock"};
>   for( auto Request : Server.RequestQueue() ){
>     auto& Response = Request.Response;
>     Response.Set( HTTP::StatusCode::OK )
>             .Set( HTTP::ContentType::Text::HTML )
>             .Append( "<html><b>{}</b></html>"_FMT( "Hello World" ) );
>   }
> }
> ```

> # How About Multithreading
> ```c++
> #include "include/EasyFCGI.hpp"
> #include <vector>
> #include <thread>
> 
> auto PretendThreadPool = std::vector<std::jthread>{};
> auto& Workers = PretendThreadPool;
> 
> int main(){
>   auto Server = EasyFCGI::Server{"path/to/unix/socket.sock"};
>   for( auto Request : Server.RequestQueue() ){
>     Workers.emplace_back( [Request = std::move( Request )] mutable {
>       auto& Response = Request.Response;
>       Workload();
>       Response.Set( HTTP::StatusCode::OK )
>               .Set( HTTP::ContentType::Text::HTML )
>               .Append( "<html><b>{}</b></html>"_FMT( "Hello World" ) );
>     } );
>   }
> }
> ```

---

> # Requirements
> - Compiler with C++26 support
> - libfcgi
> - nlohmann/json

---
