# The image-attachment subsystem: content-addressed image storage with
# signature/dimension validation. OpenSSL supplies the sha256 attachment id.
find_package(OpenSSL REQUIRED)

araya_add_plugin(attachment
    SOURCES src/attachment.cpp
    DEPS araya::araya OpenSSL::Crypto
    TEST tests/attachment_test.cpp
)
