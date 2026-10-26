#include "TokenStore.hpp"
#include <cassert>

int main() {
    webengine::TokenStore tokens;
    const auto active = tokens.issue("sample-user", webengine::Role::Viewer,
                                     std::chrono::hours(1), 7);
    const auto expired = tokens.issue("sample-user", webengine::Role::Viewer,
                                      std::chrono::seconds(-1), 7);
    assert(!tokens.validate(expired));
    assert(tokens.size() == 2);
    const auto next = tokens.issue("other-user", webengine::Role::Admin);
    assert(tokens.size() == 2);
    assert(tokens.validate(active)->revision == 7);
    assert(tokens.validate(next));
    tokens.set_revision_validator([](const std::string&) { return 8; });
    assert(!tokens.validate(active));
    tokens.revoke(next);
    assert(tokens.size() == 1);
}
