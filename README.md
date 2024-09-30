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

> # Some Useful Utilities
> ```c++
> auto RequestMethod = Request.Method;
> auto ContentType   = Request.ContentType;
> 
> auto YourFormField       = Request.Query["YourFieldName"];
> auto RepeatedFormField_N = Request.Query.CountRepeated("RepeatedFieldName");
> auto RepeatedFormField_0 = Request.Query["RepeatedFieldName",0];
> auto RepeatedFormField_1 = Request.Query["RepeatedFieldName",1];
> 
> auto UploadFile     = Request.Files["FileFieldName",2];
> auto UploadFileName = UploadFile.FileName;
> auto UploadFileSize = UploadFile.ContentBody.size();
> auto SavedLocation  = UploadFile.SaveAs( "your/save/destination" );
>
> auto CustomHeader = Request.Header["Custom-Header"];
> auto MyCookie123  = Request.Cookie["MyCookie123"];
>
> auto& Response = Request.Response;
> Response.Set( HTTP::ContentType::Text::HTML );
> Response.SetCookie( "MyCookie123", "haha; Max-Age=3600" );
> Response.Cookie["MyOtherCookie123"] = "foobar; Secure; HttpOnly";
> Response.SetHeader( "Some-Random-Header", "blahblahaabl" );
> Response.Header["Some-Other-Random-Header"] = "asdfjbkljsdabf";
> ```
---

> # Requirements
> - Compiler with C++26 support
> - libfcgi
> - nlohmann/json

---
