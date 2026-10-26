#include "webengine/NginxController.hpp"
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>
namespace fs = std::filesystem;
using namespace std::chrono_literals;
unsigned short port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof address)==0);
    socklen_t length=sizeof address; assert(getsockname(fd,reinterpret_cast<sockaddr*>(&address),&length)==0);
    close(fd); return ntohs(address.sin_port);
}
void identity(const fs::path& certfile,const fs::path& keyfile,long serial) {
    EVP_PKEY_CTX* context=EVP_PKEY_CTX_new_id(EVP_PKEY_RSA,nullptr); EVP_PKEY* key{};
    assert(context && EVP_PKEY_keygen_init(context)==1 && EVP_PKEY_CTX_set_rsa_keygen_bits(context,2048)==1 && EVP_PKEY_keygen(context,&key)==1); EVP_PKEY_CTX_free(context);
    X509* cert=X509_new(); assert(cert); X509_set_version(cert,2); ASN1_INTEGER_set(X509_get_serialNumber(cert),serial);
    X509_gmtime_adj(X509_getm_notBefore(cert),-60); X509_gmtime_adj(X509_getm_notAfter(cert),86400);
    X509_NAME* name=X509_get_subject_name(cert); X509_NAME_add_entry_by_txt(name,"CN",MBSTRING_ASC,reinterpret_cast<const unsigned char*>("localhost"),-1,-1,0);
    X509_set_issuer_name(cert,name); X509_set_pubkey(cert,key); assert(X509_sign(cert,key,EVP_sha256())>0);
    FILE* file=fopen(certfile.c_str(),"w"); assert(file && PEM_write_X509(file,cert)==1); fclose(file);
    file=fopen(keyfile.c_str(),"w"); assert(file && PEM_write_PrivateKey(file,key,nullptr,nullptr,0,nullptr,nullptr)==1); fclose(file);
    X509_free(cert); EVP_PKEY_free(key);
}
long served(unsigned short portnum) {
    int fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0) return -1;
    timeval timeout{1,0}; setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout); setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
    sockaddr_in address{}; address.sin_family=AF_INET; address.sin_port=htons(portnum); address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof address)) {close(fd);return -1;}
    SSL_CTX* context=SSL_CTX_new(TLS_client_method()); SSL* ssl=SSL_new(context); SSL_set_fd(ssl,fd);
    long result=-1;
    if(SSL_connect(ssl)==1) {X509* cert=SSL_get1_peer_certificate(ssl);if(cert){result=ASN1_INTEGER_get(X509_get_serialNumber(cert));X509_free(cert);}}
    SSL_free(ssl); SSL_CTX_free(context); close(fd); return result;
}
bool wait_for(unsigned short portnum,long serial) {for(int n=0;n<100;++n) {if(served(portnum)==serial)return true;std::this_thread::sleep_for(20ms);}return false;}
int main() {
    const char* executable=std::getenv("WEBENGINE_NGINX"); if(!executable){std::cerr<<"Set WEBENGINE_NGINX to run nginx TLS integration\n";return 77;}
    char temporary[]="/tmp/webengine-tls-XXXXXX"; assert(mkdtemp(temporary)); fs::path root=temporary;
    identity(root/"a.crt",root/"a.key",1); identity(root/"b.crt",root/"b.key",2);
    webengine::NginxController::Options options; options.nginx_bin=executable; options.config=root/"nginx.conf"; options.pidfile=root/"nginx.pid";
    options.listen_file=root/"listen.conf"; options.tls_file=root/"tls.conf"; options.temp_root=root/"temp"; options.http_port=port(); do {options.https_port=port();}while(options.https_port==options.http_port);
    std::ofstream(options.config)<<"worker_processes 1; pid "<<options.pidfile<<"; error_log stderr; events {} http { access_log off; client_body_temp_path "<<root.string()<<"/temp/client_body; proxy_temp_path "<<root.string()<<"/temp/proxy; fastcgi_temp_path "<<root.string()<<"/temp/fastcgi; uwsgi_temp_path "<<root.string()<<"/temp/uwsgi; scgi_temp_path "<<root.string()<<"/temp/scgi; server { include "<<options.listen_file<<"; include "<<options.tls_file<<"; return 200 'ok'; } }\n";
    webengine::NginxController controller(options);
    struct Cleanup {webengine::NginxController& c;fs::path root;~Cleanup(){c.off();std::this_thread::sleep_for(100ms);fs::remove_all(root);}}cleanup{controller,root};
    auto a=webengine::WebServerController::TlsIdentity{(root/"a.crt").string(),(root/"a.key").string(),false};
    auto b=webengine::WebServerController::TlsIdentity{(root/"b.crt").string(),(root/"b.key").string(),false};
    assert(controller.set_tls_identity(a)); assert(controller.on()); assert(wait_for(options.https_port,1));
    assert(controller.set_tls_identity(b,[&]{return wait_for(options.https_port,2);}));
    assert(!controller.set_tls_identity(a,[]{return false;})); assert(!controller.last_error().empty()); assert(wait_for(options.https_port,2));
    auto bad=b;bad.private_key=(root/"missing.key").string();assert(!controller.set_tls_identity(bad));assert(wait_for(options.https_port,2));
    bad=b;bad.private_key="x\"; include injected;\n";assert(!controller.set_tls_identity(bad));assert(wait_for(options.https_port,2));
    std::promise<void> entered,release; auto released=release.get_future().share();
    auto activation=std::async(std::launch::async,[&]{return controller.set_tls_identity(a,[&]{entered.set_value();released.wait();return wait_for(options.https_port,1);});});
    entered.get_future().wait();
    auto concurrent=std::async(std::launch::async,[&]{return controller.test_config();});
    assert(concurrent.wait_for(100ms)==std::future_status::timeout);release.set_value();assert(activation.get());assert(concurrent.get());
    assert(wait_for(options.https_port,1));
}
