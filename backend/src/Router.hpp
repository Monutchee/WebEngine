#pragma once
#include <boost/beast/http.hpp>
#include "AclStore.hpp"
#include "TokenStore.hpp"
#include "AuthHandler.hpp"
#include "util.hpp"

namespace http = boost::beast::http;

class Router {
public:
    Router(AclStore& acl, TokenStore& tokens)
        : acl_(acl), auth_(acl, tokens) {}

    http::response<http::string_body>
    dispatch(const http::request<http::string_body>& req);

private:
    AclStore&   acl_;
    AuthHandler auth_;
};
