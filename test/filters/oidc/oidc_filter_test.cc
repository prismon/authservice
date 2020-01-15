#include "src/filters/oidc/oidc_filter.h"
#include <regex>
#include "absl/strings/str_join.h"
#include "google/rpc/code.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "src/common/http/headers.h"
#include "test/common/http/mocks.h"
#include "test/common/session/mocks.h"
#include "test/filters/oidc/mocks.h"
#include "src/filters/oidc/in_memory_session_store.h"

namespace envoy {
namespace api {
namespace v2 {
namespace core {

// Used for printing header information on test failures
void PrintTo(const ::envoy::api::v2::core::HeaderValueOption &header, ::std::ostream *os) {
  std::string json;
  google::protobuf::util::MessageToJsonString(header, &json);

  *os << json;
}

}
}
}
}

namespace authservice {
namespace filters {
namespace oidc {

using ::testing::_;
using ::testing::Eq;
using ::testing::StrEq;
using ::testing::AnyOf;
using ::testing::AllOf;
using ::testing::Return;
using ::testing::ByMove;
using ::testing::Property;
using ::testing::StartsWith;
using ::testing::MatchesRegex;
using ::testing::UnorderedElementsAre;

namespace {

::testing::internal::UnorderedElementsAreArrayMatcher<::testing::Matcher<envoy::api::v2::core::HeaderValueOption>>
ContainsHeaders(std::vector<std::pair<std::string, ::testing::Matcher<std::string>>> headers) {
  std::vector<::testing::Matcher<envoy::api::v2::core::HeaderValueOption>> matchers;

  for(const auto& header : headers) {
    matchers.push_back(
      Property(&envoy::api::v2::core::HeaderValueOption::header, AllOf(
        Property(&envoy::api::v2::core::HeaderValue::key, StrEq(header.first)),
        Property(&envoy::api::v2::core::HeaderValue::value, header.second)
      )));
  }

  return ::testing::UnorderedElementsAreArray(matchers);
}

}

class OidcFilterTest : public ::testing::Test {
 protected:
  authservice::config::oidc::OIDCConfig config_;
  std::string callback_host_;
  std::shared_ptr<TokenResponseParserMock> parser_mock_;
  std::shared_ptr<common::session::TokenEncryptorMock> cryptor_mock_;
  std::shared_ptr<common::session::SessionIdGeneratorMock> session_id_generator_mock_;
  std::shared_ptr<SessionStore> session_store_;
  std::shared_ptr<TokenResponse> test_token_response_;

  const char* test_jwt_string_ = "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIiwibmFtZSI6IkpvaG4gRG9lIiwiYWRtaW4iOnRydWUsImlhdCI6MTUxNjIzOTAyMiwiYXVkIjpbImNsaWVudDEiXSwibm9uY2UiOiJyYW5kb20ifQ.NQi_VTRjZ8jv5cAp4inpuQ9STfVgCoWfONjLnZEMk8la8s99J9b6QmcKtO2tabTgvcseikVNlPuB6fZztY_fxhdrNE0dBNAl1lhz_AWBz6Yr-D82LLKk5NQ-IKDloF19Pic0Ub9pGCqNLOlmRXRVcfwwq5nISzfP6OdrjepRZ2Jd3rc2HvHYm-6GstH4xkKViABVwCDmwlAOi47bdHPByHkZOOnHSQEElr4tqO_uAQRpj36Yvt-95nPKhWaufZhcpYKk1H7ZRmylJQuG_dhlw4gN1i5iWBMk-Sj_2xyk05Bap1qkKSeHTxyqzhtDAH0LHYZdo_2hU-7YnL4JRhVVwg";
  google::jwt_verify::Jwt test_jwt_;

  void SetUp() override {
    config_.mutable_authorization()->set_scheme("https");
    config_.mutable_authorization()->set_hostname("acme-idp.tld");
    config_.mutable_authorization()->set_port(443);
    config_.mutable_authorization()->set_path("/authorization");
    config_.mutable_token()->set_scheme("https");
    config_.mutable_token()->set_hostname("acme-idp.tld");
    config_.mutable_token()->set_port(443);
    config_.mutable_token()->set_path("/token");
    config_.mutable_jwks_uri()->set_scheme("https");
    config_.mutable_jwks_uri()->set_hostname("acme-idp.tld");
    config_.mutable_jwks_uri()->set_port(443);
    config_.mutable_jwks_uri()->set_path("/token");
    config_.set_jwks("some-jwks");
    config_.mutable_callback()->set_scheme("https");
    config_.mutable_callback()->set_hostname("me.tld");
    config_.mutable_callback()->set_port(443);
    config_.mutable_callback()->set_path("/callback");
    config_.set_client_id("example-app");
    config_.set_client_secret("ZXhhbXBsZS1hcHAtc2VjcmV0");
    config_.set_cryptor_secret("xxx123");
    config_.set_landing_page("/landing-page");
    config_.set_cookie_name_prefix("cookie-prefix");
    config_.mutable_id_token()->set_header("authorization");
    config_.mutable_id_token()->set_preamble("Bearer");
    config_.set_timeout(300);

    std::stringstream callback_host;
    callback_host << config_.callback().hostname() << ':' << std::dec << config_.callback().port();
    callback_host_ = callback_host.str();

    parser_mock_ = std::make_shared<TokenResponseParserMock>();
    cryptor_mock_ = std::make_shared<common::session::TokenEncryptorMock>();
    session_id_generator_mock_ = std::make_shared<common::session::SessionIdGeneratorMock>();
    session_store_ = std::static_pointer_cast<SessionStore>(std::make_shared<InMemorySessionStore>());

    auto jwt_status = test_jwt_.parseFromString(test_jwt_string_);
    ASSERT_EQ(jwt_status, google::jwt_verify::Status::Ok);

    test_token_response_ = std::make_shared<TokenResponse>(test_jwt_);
    test_token_response_->SetAccessToken("expected_access_token");
    test_token_response_->SetExpiry(42);
  }

  void RetrieveToken(config::oidc::OIDCConfig &oidcConfig, std::string callback_host_on_request);
};

TEST_F(OidcFilterTest, Constructor) {
  OidcFilter filter(common::http::ptr_t(), config_, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);
}

TEST_F(OidcFilterTest, Name) {
  OidcFilter filter(common::http::ptr_t(), config_, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);
  ASSERT_EQ(filter.Name().compare("oidc"), 0);
}

TEST_F(OidcFilterTest, GetStateCookieName) {
  config_.clear_cookie_name_prefix();
  OidcFilter filter1(common::http::ptr_t(), config_, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);
  ASSERT_EQ(filter1.GetStateCookieName(), "__Host-authservice-state-cookie");

  config_.set_cookie_name_prefix("my-prefix");
  OidcFilter filter2(common::http::ptr_t(), config_, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);
  ASSERT_EQ(filter2.GetStateCookieName(),
            "__Host-my-prefix-authservice-state-cookie");
}

TEST_F(OidcFilterTest, GetSessionIdCookieName) {
  config_.clear_cookie_name_prefix();
  OidcFilter filter1(common::http::ptr_t(), config_, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);
  ASSERT_EQ(filter1.GetSessionIdCookieName(),
            "__Host-authservice-session-id-cookie");

  config_.set_cookie_name_prefix("my-prefix");
  OidcFilter filter2(common::http::ptr_t(), config_, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);
  ASSERT_EQ(filter2.GetSessionIdCookieName(),
            "__Host-my-prefix-authservice-session-id-cookie");
}

TEST_F(OidcFilterTest, NoHttpHeader) {
  OidcFilter filter(common::http::ptr_t(), config_, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);

  ::envoy::service::auth::v2::CheckRequest request;
  ::envoy::service::auth::v2::CheckResponse response;
  auto status = filter.Process(&request, &response);
  ASSERT_EQ(status, google::rpc::Code::INVALID_ARGUMENT);
}

/* TODO: Reinstate
TEST_F(OidcFilterTest, NoHttpSchema) {
  OidcFilter filter(common::http::ptr_t(), config);
  ::envoy::service::auth::v2::CheckRequest request;
  ::envoy::service::auth::v2::CheckResponse response;
  auto status = filter.Process(&request, &response);
  ASSERT_EQ(status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT);
}
 */

TEST_F(OidcFilterTest, NoAuthorization) {
  EXPECT_CALL(*cryptor_mock_, Encrypt(_)).WillOnce(Return("encrypted"));
  EXPECT_CALL(*session_id_generator_mock_, Generate()).WillOnce(Return("session123"));
  OidcFilter filter(common::http::ptr_t(), config_, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);
  ::envoy::service::auth::v2::CheckRequest request;
  ::envoy::service::auth::v2::CheckResponse response;
  auto httpRequest = request.mutable_attributes()->mutable_request()->mutable_http();
  httpRequest->set_scheme("https");

  auto status = filter.Process(&request, &response);
  ASSERT_EQ(status, google::rpc::Code::UNAUTHENTICATED);
  ASSERT_EQ(response.denied_response().status().code(),
            ::envoy::type::StatusCode::Found);

  ASSERT_THAT(
    response.denied_response().headers(),
    ContainsHeaders({
      {
        common::http::headers::Location,
        MatchesRegex(
          "^https://acme-idp\\.tld/"
          "authorization\\?client_id=example-app&nonce=[A-Za-z0-9_-]{43}&"
          "redirect_uri=https%3A%2F%2Fme\\.tld%2Fcallback&response_type=code&"
          "scope=openid&state=[A-Za-z0-9_-]{43}$")
      },
      {common::http::headers::CacheControl, StrEq(common::http::headers::CacheControlDirectives::NoCache)},
      {common::http::headers::Pragma, StrEq(common::http::headers::PragmaDirectives::NoCache)},
      {
       common::http::headers::SetCookie,
        StrEq("__Host-cookie-prefix-authservice-state-cookie=encrypted; "
              "HttpOnly; Max-Age=300; Path=/; "
              "SameSite=Lax; Secure")
      },
      {
       common::http::headers::SetCookie,
        StrEq("__Host-cookie-prefix-authservice-session-id-cookie=session123; "
              "HttpOnly; Path=/; "
              "SameSite=Lax; Secure")
      }
    })
  );
}

TEST_F(OidcFilterTest, NoSessionCookie) {
  EXPECT_CALL(*cryptor_mock_, Encrypt(_)).WillOnce(Return("encrypted"));
  EXPECT_CALL(*session_id_generator_mock_, Generate()).WillOnce(Return("session123"));
  OidcFilter filter(common::http::ptr_t(), config_, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);
  ::envoy::service::auth::v2::CheckRequest request;
  ::envoy::service::auth::v2::CheckResponse response;
  auto httpRequest =
      request.mutable_attributes()->mutable_request()->mutable_http();
  httpRequest->set_scheme("https");
  httpRequest->mutable_headers()->insert(
      {common::http::headers::Cookie, "invalid"});

  auto status = filter.Process(&request, &response);
  // We expect to be redirected to authenticate
  ASSERT_EQ(status, google::rpc::Code::UNAUTHENTICATED);

  ASSERT_THAT(
    response.denied_response().headers(),
    ContainsHeaders({
      {common::http::headers::Location, StartsWith(common::http::http::ToUrl(config_.authorization()))},
      {common::http::headers::CacheControl, StrEq(common::http::headers::CacheControlDirectives::NoCache)},
      {common::http::headers::Pragma, StrEq(common::http::headers::PragmaDirectives::NoCache)},
      {
        common::http::headers::SetCookie,
        StrEq("__Host-cookie-prefix-authservice-state-cookie=encrypted; "
              "HttpOnly; Max-Age=300; Path=/; "
              "SameSite=Lax; Secure")
      },
      {
        common::http::headers::SetCookie,
        StrEq("__Host-cookie-prefix-authservice-session-id-cookie=session123; "
              "HttpOnly; Path=/; "
              "SameSite=Lax; Secure")
      }
    })
  );
}

// TODO write a test for when the ID token has expired (based on the exp claim in the JWT)
// TODO write a test for when the access token has expired (based on the expiry time in the json response)
// We expect to be redirected to authenticate

TEST_F(OidcFilterTest, AlreadyHasUnexpiredIdTokenShouldSendRequestToAppWithAuthorizationHeaderContainingIdToken) { // TODO simulate the "unexpiredness" of the id token
  OidcFilter filter(common::http::ptr_t(), config_, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);
  session_store_->set("session123", *test_token_response_);
  ::envoy::service::auth::v2::CheckRequest request;
  ::envoy::service::auth::v2::CheckResponse response;
  auto httpRequest =
      request.mutable_attributes()->mutable_request()->mutable_http();
  httpRequest->set_scheme("https");
  httpRequest->mutable_headers()->insert(
      {common::http::headers::Cookie,
       "__Host-cookie-prefix-authservice-session-id-cookie=session123"});

  auto status = filter.Process(&request, &response);
  ASSERT_EQ(status, google::rpc::Code::OK);

  ASSERT_THAT(
    response.ok_response().headers(),
    ContainsHeaders({
      {common::http::headers::Authorization, StrEq("Bearer " + std::string(test_jwt_string_))},
    })
  );
}

TEST_F(OidcFilterTest, MissingAccessTokenShouldRedirectToIdpToAuthenticateAgainWhenTheAccessTokenHeaderHasBeenConfigured) {
  config_.mutable_access_token()->set_header("access_token");

  TokenResponse token_response(test_jwt_);
  token_response.SetExpiry(42); // TODO when we start checking expiry update this number
  session_store_->set("session123", token_response);

  EXPECT_CALL(*cryptor_mock_, Encrypt(_)).WillOnce(Return("encrypted"));
  OidcFilter filter(common::http::ptr_t(), config_, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);
  ::envoy::service::auth::v2::CheckRequest request;
  ::envoy::service::auth::v2::CheckResponse response;
  auto httpRequest =
      request.mutable_attributes()->mutable_request()->mutable_http();
  httpRequest->set_scheme("https");
  httpRequest->mutable_headers()->insert(
      {common::http::headers::Cookie,
       "__Host-cookie-prefix-authservice-session-id-cookie=session123"});

  auto status = filter.Process(&request, &response);
  // We expect to be redirected to authenticate
  ASSERT_EQ(status, google::rpc::Code::UNAUTHENTICATED);

  ASSERT_THAT(
    response.denied_response().headers(),
    ContainsHeaders({
      {common::http::headers::Location, StartsWith(common::http::http::ToUrl(config_.authorization()))},
      {common::http::headers::CacheControl, StrEq(common::http::headers::CacheControlDirectives::NoCache)},
      {common::http::headers::Pragma, StrEq(common::http::headers::PragmaDirectives::NoCache)},
      {
        common::http::headers::SetCookie,
        StrEq("__Host-cookie-prefix-authservice-state-cookie=encrypted; "
              "HttpOnly; Max-Age=300; Path=/; "
              "SameSite=Lax; Secure")
      }
    })
  );
}

TEST_F(OidcFilterTest, AlreadyHasUnexpiredTokensShouldSendRequestToAppWithHeadersContainingBothTokensWhenTheAccessTokenHeaderHasBeenConfigured) {
  config_.mutable_access_token()->set_header("access_token");
  session_store_->set("session123", *test_token_response_);
  OidcFilter filter(common::http::ptr_t(), config_, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);
  ::envoy::service::auth::v2::CheckRequest request;
  ::envoy::service::auth::v2::CheckResponse response;
  auto httpRequest =
      request.mutable_attributes()->mutable_request()->mutable_http();
  httpRequest->set_scheme("https");
  httpRequest->mutable_headers()->insert(
      {common::http::headers::Cookie,
       "__Host-cookie-prefix-authservice-session-id-cookie=session123"});

  auto status = filter.Process(&request, &response);
  ASSERT_EQ(status, google::rpc::Code::OK);

  ASSERT_THAT(
    response.ok_response().headers(),
    ContainsHeaders({
      {common::http::headers::Authorization, StrEq("Bearer " + std::string(test_jwt_string_))},
      {"access_token", StrEq("expected_access_token")},
    })
  );
}

TEST_F(OidcFilterTest, LogoutWithCookies) {
  session_store_->set("session123", *test_token_response_);
  config_.mutable_logout()->set_path("/logout");
  config_.mutable_logout()->set_redirect_to_uri("https://redirect-uri");
  OidcFilter filter(common::http::ptr_t(), config_, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);
  ::envoy::service::auth::v2::CheckRequest request;
  ::envoy::service::auth::v2::CheckResponse response;
  auto httpRequest =
      request.mutable_attributes()->mutable_request()->mutable_http();
  httpRequest->set_scheme("https");
  httpRequest->mutable_headers()->insert(
      {common::http::headers::Cookie,
       "__Host-cookie-prefix-authservice-state-cookie=state; "
       "__Host-cookie-prefix-authservice-session-id-cookie=session123"
      });
  httpRequest->set_path("/logout");

  auto status = filter.Process(&request, &response);

  ASSERT_FALSE(session_store_->get("session123").has_value());

  ASSERT_EQ(status, google::rpc::Code::UNAUTHENTICATED);
  ASSERT_EQ(response.denied_response().status().code(),
            ::envoy::type::StatusCode::Found);

  ASSERT_THAT(
      response.denied_response().headers(),
      ContainsHeaders({
        {common::http::headers::Location, StrEq("https://redirect-uri")},
        {common::http::headers::CacheControl, StrEq(common::http::headers::CacheControlDirectives::NoCache)},
        {common::http::headers::Pragma, StrEq(common::http::headers::PragmaDirectives::NoCache)},
        {common::http::headers::SetCookie, StrEq(
            "__Host-cookie-prefix-authservice-state-cookie=deleted; HttpOnly; Max-Age=0; Path=/; SameSite=Lax; Secure")},
        {common::http::headers::SetCookie, StrEq(
            "__Host-cookie-prefix-authservice-session-id-cookie=deleted; HttpOnly; Max-Age=0; Path=/; SameSite=Lax; Secure")}
    })
  );
}

TEST_F(OidcFilterTest, LogoutWithNoCookies) {
  config_.mutable_logout()->set_path("/logout");
  config_.mutable_logout()->set_redirect_to_uri("https://redirect-uri");
  OidcFilter filter(common::http::ptr_t(), config_, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);
  ::envoy::service::auth::v2::CheckRequest request;
  ::envoy::service::auth::v2::CheckResponse response;
  auto httpRequest =
      request.mutable_attributes()->mutable_request()->mutable_http();
  httpRequest->set_scheme("https");
  httpRequest->set_path("/logout");

  auto status = filter.Process(&request, &response);

  ASSERT_EQ(status, google::rpc::Code::UNAUTHENTICATED);
  ASSERT_EQ(response.denied_response().status().code(),
            ::envoy::type::StatusCode::Found);

  ASSERT_THAT(
      response.denied_response().headers(),
      ContainsHeaders({
                          {common::http::headers::Location, StrEq("https://redirect-uri")},
                          {common::http::headers::CacheControl, StrEq(common::http::headers::CacheControlDirectives::NoCache)},
                          {common::http::headers::Pragma, StrEq(common::http::headers::PragmaDirectives::NoCache)},
                          {common::http::headers::SetCookie, StrEq(
                              "__Host-cookie-prefix-authservice-state-cookie=deleted; HttpOnly; Max-Age=0; Path=/; SameSite=Lax; Secure")},
                          {common::http::headers::SetCookie, StrEq(
                              "__Host-cookie-prefix-authservice-session-id-cookie=deleted; HttpOnly; Max-Age=0; Path=/; SameSite=Lax; Secure")}
                      })
  );
}

void OidcFilterTest::RetrieveToken(config::oidc::OIDCConfig &oidcConfig, std::string callback_host_on_request) {
  EXPECT_CALL(*parser_mock_, Parse(oidcConfig.client_id(), ::testing::_, ::testing::_))
      .WillOnce(::testing::Return(*test_token_response_));
  auto mocked_http = new common::http::http_mock();
  auto raw_http = common::http::response_t(
      new beast::http::response<beast::http::string_body>());
  raw_http->result(beast::http::status::ok);
  EXPECT_CALL(*mocked_http, Post(_, _, _, _, _))
      .WillOnce(Return(ByMove(std::move(raw_http))));
  OidcFilter filter(common::http::ptr_t(mocked_http), oidcConfig, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);
  ::envoy::service::auth::v2::CheckRequest request;
  ::envoy::service::auth::v2::CheckResponse response;
  auto httpRequest = request.mutable_attributes()->mutable_request()->mutable_http();
  httpRequest->set_scheme("");  // Seems like it should be "https", but in practice is empty
  httpRequest->set_host(callback_host_on_request);
  httpRequest->mutable_headers()->insert(
      {common::http::headers::Cookie,
       "__Host-cookie-prefix-authservice-state-cookie=valid; "
       "__Host-cookie-prefix-authservice-session-id-cookie=session123"});
  EXPECT_CALL(*cryptor_mock_, Decrypt("valid"))
      .WillOnce(Return(
          absl::optional<std::string>("expectedstate;expectednonce")));
  std::vector<absl::string_view> parts = {oidcConfig.callback().path().c_str(),
                                          "code=value&state=expectedstate"};
  httpRequest->set_path(absl::StrJoin(parts, "?"));

  auto code = filter.Process(&request, &response);
  ASSERT_EQ(code, google::rpc::Code::UNAUTHENTICATED);

  auto stored_token_response = session_store_->get("session123");
  ASSERT_TRUE(stored_token_response.has_value());
  ASSERT_EQ(stored_token_response.value().IDToken().jwt_, test_jwt_string_);
  ASSERT_EQ(stored_token_response.value().AccessToken(), "expected_access_token");
  ASSERT_EQ(stored_token_response.value().Expiry(), 42);

  ASSERT_THAT(
    response.denied_response().headers(),
    ContainsHeaders({
      {common::http::headers::Location, StartsWith(oidcConfig.landing_page())},
      {common::http::headers::CacheControl, StrEq(common::http::headers::CacheControlDirectives::NoCache)},
      {common::http::headers::Pragma, StrEq(common::http::headers::PragmaDirectives::NoCache)},
      {
        common::http::headers::SetCookie,
        StrEq("__Host-cookie-prefix-authservice-state-cookie=deleted; "
              "HttpOnly; Max-Age=0; Path=/; SameSite=Lax; "
              "Secure")
      }
    })
  );
}

TEST_F(OidcFilterTest, RetrieveTokenWithoutAccessTokenHeaderNameConfigured) {
  RetrieveToken(config_, callback_host_);
}

TEST_F(OidcFilterTest, RetrieveTokenWithoutAccessTokenHeaderNameConfiguredWhenThePortIsNotInTheRequestHostnameAndTheConfiguredCallbackIsTheDefaultHttpsPort) {
  config_.mutable_callback()->set_scheme("https");
  config_.mutable_callback()->set_port(443);
  RetrieveToken(config_, config_.callback().hostname());
}

TEST_F(OidcFilterTest, RetrieveTokenWithoutAccessTokenHeaderNameConfiguredWhenThePortIsNotInTheRequestHostnameAndTheConfiguredCallbackIsTheDefaultHttpPort) {
  config_.mutable_callback()->set_scheme("http");
  config_.mutable_callback()->set_port(80);
  RetrieveToken(config_, config_.callback().hostname());
}

TEST_F(OidcFilterTest, RetrieveTokenWithAccessTokenHeaderNameConfigured) {
  config_.mutable_access_token()->set_header("access_token");
  google::jwt_verify::Jwt jwt = {};
  auto token_response = absl::make_optional<TokenResponse>(jwt);
  token_response->SetAccessToken("expected_access_token");
  EXPECT_CALL(*parser_mock_, Parse(config_.client_id(), ::testing::_, ::testing::_))
      .WillOnce(::testing::Return(token_response));
  auto mocked_http = new common::http::http_mock();
  auto raw_http = common::http::response_t(
      new beast::http::response<beast::http::string_body>());
  raw_http->result(beast::http::status::ok);
  EXPECT_CALL(*mocked_http, Post(_, _, _, _, _))
      .WillOnce(Return(ByMove(std::move(raw_http))));
  OidcFilter filter(common::http::ptr_t(mocked_http), config_, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);
  ::envoy::service::auth::v2::CheckRequest request;
  ::envoy::service::auth::v2::CheckResponse response;
  auto httpRequest = request.mutable_attributes()->mutable_request()->mutable_http();
  httpRequest->set_scheme("");  // Seems like it should be "https", but in practice is empty
  httpRequest->set_host(callback_host_);
  httpRequest->mutable_headers()->insert(
      {common::http::headers::Cookie,
       "__Host-cookie-prefix-authservice-state-cookie=valid; "
       "__Host-cookie-prefix-authservice-session-id-cookie=session123"});
  EXPECT_CALL(*cryptor_mock_, Decrypt("valid"))
      .WillOnce(Return(
          absl::optional<std::string>("expectedstate;expectednonce")));
  std::vector<absl::string_view> parts = {config_.callback().path().c_str(),
                                          "code=value&state=expectedstate"};
  httpRequest->set_path(absl::StrJoin(parts, "?"));
  auto code = filter.Process(&request, &response);
  ASSERT_EQ(code, google::rpc::Code::UNAUTHENTICATED);

  ASSERT_THAT(
    response.denied_response().headers(),
    ContainsHeaders({
      {common::http::headers::Location, StartsWith(config_.landing_page())},
      {common::http::headers::CacheControl, StrEq(common::http::headers::CacheControlDirectives::NoCache)},
      {common::http::headers::Pragma, StrEq(common::http::headers::PragmaDirectives::NoCache)},
      {
        common::http::headers::SetCookie,
        StrEq("__Host-cookie-prefix-authservice-state-cookie=deleted; "
              "HttpOnly; Max-Age=0; Path=/; SameSite=Lax; "
              "Secure")
      }
    })
  );
}

TEST_F(OidcFilterTest, RetrieveTokenWhenTokenResponseIsMissingAccessToken) {
  config_.mutable_access_token()->set_header("access_token");
  google::jwt_verify::Jwt jwt = {};
  auto token_response = absl::make_optional<TokenResponse>(jwt);
  EXPECT_CALL(*parser_mock_, Parse(config_.client_id(), ::testing::_, ::testing::_))
      .WillOnce(::testing::Return(token_response));
  auto mocked_http = new common::http::http_mock();
  auto raw_http = common::http::response_t(
      new beast::http::response<beast::http::string_body>());
  raw_http->result(beast::http::status::ok);
  EXPECT_CALL(*mocked_http, Post(_, _, _, _, _))
      .WillOnce(Return(ByMove(std::move(raw_http))));
  ASSERT_FALSE(session_store_->get("session123").has_value());
  OidcFilter filter(common::http::ptr_t(mocked_http), config_, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);
  ::envoy::service::auth::v2::CheckRequest request;
  ::envoy::service::auth::v2::CheckResponse response;
  auto httpRequest =
      request.mutable_attributes()->mutable_request()->mutable_http();
  httpRequest->set_scheme(
      "");  // Seems like it should be "https", but in practice is empty
  httpRequest->set_host(callback_host_);
  httpRequest->mutable_headers()->insert(
      {common::http::headers::Cookie,
       "__Host-cookie-prefix-authservice-state-cookie=valid; "
       "__Host-cookie-prefix-authservice-session-id-cookie=session123"});
  EXPECT_CALL(*cryptor_mock_, Decrypt("valid"))
      .WillOnce(Return(
          absl::optional<std::string>("expectedstate;expectednonce")));
  EXPECT_CALL(*cryptor_mock_, Encrypt(_)).Times(0);
  std::vector<absl::string_view> parts = {config_.callback().path().c_str(),
                                          "code=value&state=expectedstate"};
  httpRequest->set_path(absl::StrJoin(parts, "?"));
  auto code = filter.Process(&request, &response);
  ASSERT_EQ(code, google::rpc::Code::INVALID_ARGUMENT);

  ASSERT_FALSE(session_store_->get("session123").has_value());
  ASSERT_THAT(
    response.denied_response().headers(),
    ContainsHeaders({
      {common::http::headers::CacheControl, StrEq(common::http::headers::CacheControlDirectives::NoCache)},
      {common::http::headers::Pragma, StrEq(common::http::headers::PragmaDirectives::NoCache)},
      {
        common::http::headers::SetCookie,
        StrEq("__Host-cookie-prefix-authservice-state-cookie=deleted; "
              "HttpOnly; Max-Age=0; Path=/; "
              "SameSite=Lax; Secure"),
      },
    })
  );
}

TEST_F(OidcFilterTest, RetrieveTokenMissingStateCookie) {
  auto mocked_http = new common::http::http_mock();
  OidcFilter filter(common::http::ptr_t(mocked_http), config_, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);
  ::envoy::service::auth::v2::CheckRequest request;
  ::envoy::service::auth::v2::CheckResponse response;
  auto httpRequest =
      request.mutable_attributes()->mutable_request()->mutable_http();
  httpRequest->set_scheme("https");
  httpRequest->set_host(callback_host_);
  httpRequest->mutable_headers()->insert(
      {common::http::headers::Cookie,
       "__Host-cookie-prefix-authservice-session-id-cookie=session123"});
  std::vector<absl::string_view> parts = {config_.callback().path().c_str(),
                                          "code=value&state=expectedstate"};
  httpRequest->set_path(absl::StrJoin(parts, "?"));
  auto code = filter.Process(&request, &response);
  ASSERT_EQ(code, google::rpc::Code::INVALID_ARGUMENT);

  ASSERT_THAT(
    response.denied_response().headers(),
    ContainsHeaders({
      {common::http::headers::CacheControl, StrEq(common::http::headers::CacheControlDirectives::NoCache)},
      {common::http::headers::Pragma, StrEq(common::http::headers::PragmaDirectives::NoCache)},
      {
        common::http::headers::SetCookie,
        StrEq("__Host-cookie-prefix-authservice-state-cookie=deleted; "
              "HttpOnly; Max-Age=0; Path=/; "
              "SameSite=Lax; Secure"),
      },
    })
  );
}

TEST_F(OidcFilterTest, RetrieveTokenInvalidStateCookie) {
  auto mocked_http = new common::http::http_mock();
  OidcFilter filter(common::http::ptr_t(mocked_http), config_, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);
  ::envoy::service::auth::v2::CheckRequest request;
  ::envoy::service::auth::v2::CheckResponse response;
  auto httpRequest =
      request.mutable_attributes()->mutable_request()->mutable_http();
  httpRequest->set_scheme("https");
  httpRequest->set_host(callback_host_);
  httpRequest->mutable_headers()->insert(
      {common::http::headers::Cookie,
       "__Host-cookie-prefix-authservice-state-cookie=invalid; "
       "__Host-cookie-prefix-authservice-session-id-cookie=session123"});
  EXPECT_CALL(*cryptor_mock_, Decrypt("invalid"))
      .WillOnce(Return(absl::nullopt));
  std::vector<absl::string_view> parts = {config_.callback().path().c_str(),
                                          "code=value&state=expectedstate"};
  httpRequest->set_path(absl::StrJoin(parts, "?"));
  auto code = filter.Process(&request, &response);
  ASSERT_EQ(code, google::rpc::Code::INVALID_ARGUMENT);

  ASSERT_THAT(
    response.denied_response().headers(),
    ContainsHeaders({
      {common::http::headers::CacheControl, StrEq(common::http::headers::CacheControlDirectives::NoCache)},
      {common::http::headers::Pragma, StrEq(common::http::headers::PragmaDirectives::NoCache)},
      {
        common::http::headers::SetCookie,
        StrEq("__Host-cookie-prefix-authservice-state-cookie=deleted; "
              "HttpOnly; Max-Age=0; Path=/; "
              "SameSite=Lax; Secure"),
      },
    })
  );
}

TEST_F(OidcFilterTest, RetrieveTokenInvalidStateCookieFormat) {
  auto mocked_http = new common::http::http_mock();
  OidcFilter filter(common::http::ptr_t(mocked_http), config_, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);
  ::envoy::service::auth::v2::CheckRequest request;
  ::envoy::service::auth::v2::CheckResponse response;
  auto httpRequest =
      request.mutable_attributes()->mutable_request()->mutable_http();
  httpRequest->set_scheme("https");
  httpRequest->set_host(callback_host_);
  httpRequest->mutable_headers()->insert(
      {common::http::headers::Cookie,
       "__Host-cookie-prefix-authservice-state-cookie=valid; "
       "__Host-cookie-prefix-authservice-session-id-cookie=session123"});
  EXPECT_CALL(*cryptor_mock_, Decrypt("valid"))
      .WillOnce(
          Return(absl::optional<std::string>("invalidformat")));
  std::vector<absl::string_view> parts = {config_.callback().path().c_str(),
                                          "code=value&state=expectedstate"};
  httpRequest->set_path(absl::StrJoin(parts, "?"));
  auto code = filter.Process(&request, &response);
  ASSERT_EQ(code, google::rpc::Code::INVALID_ARGUMENT);

  ASSERT_THAT(
    response.denied_response().headers(),
    ContainsHeaders({
      {common::http::headers::CacheControl, StrEq(common::http::headers::CacheControlDirectives::NoCache)},
      {common::http::headers::Pragma, StrEq(common::http::headers::PragmaDirectives::NoCache)},
      {
        common::http::headers::SetCookie,
        StrEq("__Host-cookie-prefix-authservice-state-cookie=deleted; "
              "HttpOnly; Max-Age=0; Path=/; "
              "SameSite=Lax; Secure"),
      },
    })
  );
}

TEST_F(OidcFilterTest, RetrieveTokenMissingCode) {
  OidcFilter filter(common::http::ptr_t(), config_, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);
  ::envoy::service::auth::v2::CheckRequest request;
  ::envoy::service::auth::v2::CheckResponse response;
  auto httpRequest =
      request.mutable_attributes()->mutable_request()->mutable_http();
  httpRequest->set_scheme("https");
  httpRequest->set_host(callback_host_);
  httpRequest->set_path(config_.callback().path());
  std::vector<absl::string_view> parts = {config_.callback().path().c_str(),
                                          "key=value&state=expectedstate"};
  httpRequest->set_path(absl::StrJoin(parts, "?"));
  httpRequest->mutable_headers()->insert(
      {common::http::headers::Cookie,
       "__Host-cookie-prefix-authservice-session-id-cookie=session123"});

  auto code = filter.Process(&request, &response);
  ASSERT_EQ(code, google::rpc::Code::INVALID_ARGUMENT);

  ASSERT_THAT(
    response.denied_response().headers(),
    ContainsHeaders({
      {common::http::headers::CacheControl, StrEq(common::http::headers::CacheControlDirectives::NoCache)},
      {common::http::headers::Pragma, StrEq(common::http::headers::PragmaDirectives::NoCache)},
      {
        common::http::headers::SetCookie,
        StrEq("__Host-cookie-prefix-authservice-state-cookie=deleted; "
              "HttpOnly; Max-Age=0; Path=/; "
              "SameSite=Lax; Secure"),
      },
    })
  );
}

TEST_F(OidcFilterTest, RetrieveTokenMissingState) {
  OidcFilter filter(common::http::ptr_t(), config_, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);
  ::envoy::service::auth::v2::CheckRequest request;
  ::envoy::service::auth::v2::CheckResponse response;
  auto httpRequest =
      request.mutable_attributes()->mutable_request()->mutable_http();
  httpRequest->set_scheme("https");
  httpRequest->set_host(callback_host_);
  httpRequest->set_path(config_.callback().path());
  std::vector<absl::string_view> parts = {config_.callback().path().c_str(),
                                          "code=value"};
  httpRequest->set_path(absl::StrJoin(parts, "?"));
  httpRequest->mutable_headers()->insert(
      {common::http::headers::Cookie,
       "__Host-cookie-prefix-authservice-session-id-cookie=session123"});

  auto code = filter.Process(&request, &response);
  ASSERT_EQ(code, google::rpc::Code::INVALID_ARGUMENT);

  ASSERT_THAT(
    response.denied_response().headers(),
    ContainsHeaders({
      {common::http::headers::CacheControl, StrEq(common::http::headers::CacheControlDirectives::NoCache)},
      {common::http::headers::Pragma, StrEq(common::http::headers::PragmaDirectives::NoCache)},
      {
        common::http::headers::SetCookie,
        StrEq("__Host-cookie-prefix-authservice-state-cookie=deleted; "
              "HttpOnly; Max-Age=0; Path=/; "
              "SameSite=Lax; Secure"),
      },
    })
  );
}

TEST_F(OidcFilterTest, RetrieveTokenUnexpectedState) {
  OidcFilter filter(common::http::ptr_t(), config_, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);
  ::envoy::service::auth::v2::CheckRequest request;
  ::envoy::service::auth::v2::CheckResponse response;
  auto httpRequest =
      request.mutable_attributes()->mutable_request()->mutable_http();
  httpRequest->set_scheme("https");
  httpRequest->set_host(callback_host_);
  httpRequest->set_path(config_.callback().path());
  std::vector<absl::string_view> parts = {config_.callback().path().c_str(),
                                          "code=value&state=unexpectedstate"};
  httpRequest->set_path(absl::StrJoin(parts, "?"));
  httpRequest->mutable_headers()->insert(
      {common::http::headers::Cookie,
       "__Host-cookie-prefix-authservice-session-id-cookie=session123"});

  auto code = filter.Process(&request, &response);
  ASSERT_EQ(code, google::rpc::Code::INVALID_ARGUMENT);

  ASSERT_THAT(
    response.denied_response().headers(),
    ContainsHeaders({
      {common::http::headers::CacheControl, StrEq(common::http::headers::CacheControlDirectives::NoCache)},
      {common::http::headers::Pragma, StrEq(common::http::headers::PragmaDirectives::NoCache)},
      {
        common::http::headers::SetCookie,
        StrEq("__Host-cookie-prefix-authservice-state-cookie=deleted; "
              "HttpOnly; Max-Age=0; Path=/; "
              "SameSite=Lax; Secure"),
      },
    })
  );
}

TEST_F(OidcFilterTest, RetrieveTokenBrokenPipe) {
  auto *http_mock = new common::http::http_mock();
  auto raw_http = common::http::response_t();
  EXPECT_CALL(*http_mock, Post(_, _, _, _, _))
      .WillOnce(Return(ByMove(std::move(raw_http))));
  OidcFilter filter(common::http::ptr_t(http_mock), config_, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);
  ::envoy::service::auth::v2::CheckRequest request;
  ::envoy::service::auth::v2::CheckResponse response;
  auto httpRequest =
      request.mutable_attributes()->mutable_request()->mutable_http();
  httpRequest->set_scheme("https");
  httpRequest->set_host(callback_host_);
  httpRequest->set_path(config_.callback().path());
  httpRequest->mutable_headers()->insert(
      {common::http::headers::Cookie,
       "__Host-cookie-prefix-authservice-state-cookie=valid; "
       "__Host-cookie-prefix-authservice-session-id-cookie=session123"});
  EXPECT_CALL(*cryptor_mock_, Decrypt("valid"))
      .WillOnce(Return(
          absl::optional<std::string>("expectedstate;expectednonce")));
  std::vector<absl::string_view> parts = {config_.callback().path().c_str(),
                                          "code=value&state=expectedstate"};
  httpRequest->set_path(absl::StrJoin(parts, "?"));
  auto code = filter.Process(&request, &response);
  ASSERT_EQ(code, google::rpc::Code::INTERNAL);

  ASSERT_THAT(
    response.denied_response().headers(),
    ContainsHeaders({
      {common::http::headers::CacheControl, StrEq(common::http::headers::CacheControlDirectives::NoCache)},
      {common::http::headers::Pragma, StrEq(common::http::headers::PragmaDirectives::NoCache)},
      {
        common::http::headers::SetCookie,
        StrEq("__Host-cookie-prefix-authservice-state-cookie=deleted; "
              "HttpOnly; Max-Age=0; Path=/; "
              "SameSite=Lax; Secure"),
      },
    })
  );
}

TEST_F(OidcFilterTest, RetrieveTokenInvalidResponse) {
  EXPECT_CALL(*parser_mock_, Parse(config_.client_id(), ::testing::_, ::testing::_))
      .WillOnce(::testing::Return(absl::nullopt));
  auto *http_mock = new common::http::http_mock();
  auto raw_http = common::http::response_t(
      (new beast::http::response<beast::http::string_body>()));
  EXPECT_CALL(*http_mock, Post(_, _, _, _, _))
      .WillOnce(Return(ByMove(std::move(raw_http))));
  OidcFilter filter(common::http::ptr_t(http_mock), config_, parser_mock_, cryptor_mock_, session_id_generator_mock_, session_store_);
  ::envoy::service::auth::v2::CheckRequest request;
  ::envoy::service::auth::v2::CheckResponse response;
  auto httpRequest =
      request.mutable_attributes()->mutable_request()->mutable_http();
  httpRequest->set_scheme("https");
  httpRequest->set_host(callback_host_);
  httpRequest->set_path(config_.callback().path());
  httpRequest->mutable_headers()->insert(
      {common::http::headers::Cookie,
       "__Host-cookie-prefix-authservice-state-cookie=valid; "
       "__Host-cookie-prefix-authservice-session-id-cookie=session123"});
  EXPECT_CALL(*cryptor_mock_, Decrypt("valid"))
      .WillOnce(Return(
          absl::optional<std::string>("expectedstate;expectednonce")));
  std::vector<absl::string_view> parts = {config_.callback().path().c_str(),
                                          "code=value&state=expectedstate"};
  httpRequest->set_path(absl::StrJoin(parts, "?"));
  auto code = filter.Process(&request, &response);
  ASSERT_EQ(code, google::rpc::Code::INVALID_ARGUMENT);

  ASSERT_THAT(
    response.denied_response().headers(),
    ContainsHeaders({
      {common::http::headers::CacheControl, StrEq(common::http::headers::CacheControlDirectives::NoCache)},
      {common::http::headers::Pragma, StrEq(common::http::headers::PragmaDirectives::NoCache)},
      {
        common::http::headers::SetCookie,
        StrEq("__Host-cookie-prefix-authservice-state-cookie=deleted; "
              "HttpOnly; Max-Age=0; Path=/; "
              "SameSite=Lax; Secure"),
      },
    })
  );
}

}  // namespace oidc
}  // namespace filters
}  // namespace authservice
