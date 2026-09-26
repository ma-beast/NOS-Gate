#define NOS_GATE_PARSER_TEST
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#include <strings.h>
#include "../src/win32/main.c"

int main(void)
{
    static const char sample[] =
        "<script>var ytInitialData={\"contents\":["
        "{\"videoRenderer\":{\"videoId\":\"abc123\","
        "\"thumbnail\":{\"thumbnails\":[{\"url\":\"https://i.ytimg.com/vi/abc123/default.jpg\"}]},"
        "\"title\":{\"runs\":[{\"text\":\"Rock & Roll <live>\"}]},"
        "\"ownerText\":{\"runs\":[{\"text\":\"Channel One\"}]},"
        "\"lengthText\":{\"simpleText\":\"3:21\"},"
        "\"viewCountText\":{\"simpleText\":\"123 views\"},"
        "\"publishedTimeText\":{\"simpleText\":\"today\"}}},"
        "{\"videoRenderer\":{\"videoId\":\"def456\","
        "\"thumbnail\":{\"thumbnails\":[{\"url\":\"https://i.ytimg.com/vi/def456/default.jpg?x=1\\u0026y=2\"}]},"
        "\"title\":{\"runs\":[{\"text\":\"\\u041a\\u043e\\u0442\"}]},"
        "\"ownerText\":{\"runs\":[{\"text\":\"Channel Two\"}]}}}]};</script>";
    static const char generic_sample[] =
        "<html><link rel=\"search\" type=\"application/opensearchdescription+xml\" href=\"/open.xml\">"
        "<form method=\"post\"><input type=\"hidden\" name=\"do\" value=\"search\">"
        "<input name=\"story\"><button class=\"plain-icon\" type=\"submit\"><span></span></button>"
        "<img src=\"/poster.webp\"></form>"
        "<form action=\"/find\"><input name=\"q\"><input type=\"image\" src=\"go.gif\"></form>"
        "<form><input name=\"login\"><button type=\"submit\"><span></span></button></form></html>";
    static const char google_sample[] =
        "<html><body><div>script-only shell</div></body></html>";
    static const char mode_sample[] =
        "<html><head><style>.x{color:red}</style><script>bad()</script>"
        "<link rel=\"stylesheet\" href=\"/site.css\"></head>"
        "<body><a href=\"/next\">Next</a><form action=\"/find\"><input name=\"q\"></form>"
        "<p class=\"x\" onclick=\"bad()\">Text</p></body></html>";
    static const char media_sample[] =
        "<html><head><title>Test Movie</title></head><body>"
        "<iframe src=\"https://api.synchroncode.com/embed/movie/5279\"></iframe>"
        "<script>var opts={hls: \"https://cdn.test/master.m3u8?a=1\\u0026b=2\","
        "download:'https://dl.test/get?x=1\\u0026title=Movie'};</script></body></html>";
    static const char hls_only_sample[] =
        "<html><head><title>HLS only</title></head><body><script>"
        "var opts={hls:'https://cdn.test/master.m3u8'};"
        "</script></body></html>";
    static const char iframe_sample[] =
        "<html><body>"
        "<iframe src=\"https://kodikplayer.com/serial/1/hash/720p\"></iframe>"
        "<iframe data-src=\"https://ceramet.net/bil/10682\"></iframe>"
        "<iframe src=\"https://mystery.test/embed/film/7\"></iframe>"
        "<iframe src=\"https://widgets.test/weather\"></iframe>"
        "</body></html>";
    static const char hls_master_sample[] =
        "#EXTM3U\r\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio1\",NAME=\"Stereo\",URI=\"audio/index.m3u8\"\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=350000,RESOLUTION=426x240,CODECS=\"avc1.4d4015\",AUDIO=\"audio1\"\r\n240/index.m3u8\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,CODECS=\"avc1.4d401e,mp4a.40.2\"\r\n360/index.m3u8\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1600000,RESOLUTION=1280x720,CODECS=\"avc1.64001f,mp4a.40.2\"\r\n720/index.m3u8\r\n";
    static const char hls_media_sample[] =
        "#EXTM3U\n"
        "#EXT-X-MAP:URI=\"init.mp4\"\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"https://keys.test/key.bin\"\n"
        "#EXT-X-I-FRAME-STREAM-INF:URI=\"nested.m3u8\"\n"
        "#EXTINF:6.0,\nseg-001.m4s\n"
        "#EXT-X-ENDLIST\n";
    char *result;
    char *generic;
    char *google;
    char *original;
    char *light;
    char *superlite;
    char *hls_local;
    char candidate[1024];
    char routed[1024];
    char decoded[1024];
    char field[128];
    char media_value[4096];
    char media_title[512];
    char mapped_referer[4096];
    media_catalog *media_catalog_test;
    media_catalog *hls_catalog_test;
    int media_id, hls_id, source_index;
    size_t result_length = 0;
    size_t generic_length = 0;
    size_t google_length = 0;
    size_t mode_length = 0;
    size_t hls_local_length = 0;
    const char *cookie_header;
    memset(cookie_jar, 0, sizeof(cookie_jar));
    gate_cookie_path[0] = 0;
    store_cookies("https://www.example.test/account/page",
                  "sid=one; Path=/; Secure; HttpOnly\n"
                  "theme=dark; Domain=.example.test; Path=/", 2);
    cookie_header = cookies_for("https://www.example.test/account/page");
    if (!strstr(cookie_header, "sid=one") || !strstr(cookie_header, "theme=dark")) return 52;
    cookie_header = cookies_for("http://www.example.test/account/page");
    if (strstr(cookie_header, "sid=one") || !strstr(cookie_header, "theme=dark")) return 53;
    cookie_header = cookies_for("https://sub.example.test/");
    if (strstr(cookie_header, "sid=one") || !strstr(cookie_header, "theme=dark")) return 54;
    store_cookies("https://www.example.test/account/page",
                  "sid=two; Path=/; Secure", 1);
    cookie_header = cookies_for("https://www.example.test/account/page");
    if (!strstr(cookie_header, "sid=two") || !strstr(cookie_header, "theme=dark") ||
        strstr(cookie_header, "sid=one")) return 55;
    store_cookies("https://www.example.test/",
                  "theme=; Domain=.example.test; Path=/; Max-Age=0", 1);
    cookie_header = cookies_for("https://www.example.test/");
    if (strstr(cookie_header, "theme=")) return 56;
    result = youtube_results_html(sample, sizeof(sample) - 1,
                                  "https://www.youtube.com/results?search_query=test",
                                  &result_length);
    if (!result) return 1;
    if (!strstr(result, "watch%3Fv%3Dabc123")) return 2;
    if (!strstr(result, "Rock &amp; Roll &lt;live&gt;")) return 3;
    if (!strstr(result, "Channel One")) return 4;
    if (!strstr(result, "def456%2Fmqdefault.jpg")) return 5;
    if (!strstr(result, "\xD0\x9A\xD0\xBE\xD1\x82")) return 6;
    if (result_length != strlen(result)) return 7;
    if (!old_ie_image_candidate("https://cdn.test/pic.jpg?format=webp&x=1",
                                candidate, sizeof(candidate)) ||
        strcmp(candidate, "https://cdn.test/pic.jpg")) return 8;
    if (!old_ie_image_candidate("https://cdn.test/pic.webp?x=1",
                                candidate, sizeof(candidate)) ||
        strcmp(candidate, "https://cdn.test/pic.jpg")) return 9;
    if (old_ie_image_candidate("https://cdn.test/pic.jpg",
                               candidate, sizeof(candidate))) return 10;
    if (default_mode_for_url("https://4pda.to/") != MODE_LIGHT) return 41;
    if (default_mode_for_url("http://flibusta.is/") != MODE_ORIGINAL) return 42;
    if (default_mode_for_url("https://www.youtube.com/") != MODE_SUPERLITE) return 43;
    if (!prepare_routed_target("https%3A%2F%2Fexample.test%2Fsearch?q=zx+81",
                               routed, sizeof(routed)) ||
        strcmp(routed, "https%3A%2F%2Fexample.test%2Fsearch&q=zx+81")) return 44;
    if (!decode_routed_address(routed, decoded, sizeof(decoded)) ||
        strcmp(decoded, "https://example.test/search?q=zx+81")) return 45;
    if (!mode_key_for_url("https://www.example.test:443/page", routed,
                          sizeof(routed)) || strcmp(routed, "example.test")) return 46;
    if (!prepare_routed_target(
            "https%3A%2F%2Fwww.youtube.com%2Fresults?search_query=4pda+photonq",
            routed, sizeof(routed)) ||
        strcmp(routed,
            "https%3A%2F%2Fwww.youtube.com%2Fresults&search_query=4pda+photonq"))
        return 49;
    if (!decode_routed_address(routed, decoded, sizeof(decoded)) ||
        strcmp(decoded,
            "https://www.youtube.com/results?search_query=4pda+photonq"))
        return 50;
    if (!prepare_routed_target(
            "https%3A%2F%2Fexample.test%2Fsearch?q=%D0%BA%D0%BE%D1%82+dog%3B",
            routed, sizeof(routed)) ||
        !decode_routed_address(routed, decoded, sizeof(decoded)) ||
        strcmp(decoded,
            "https://example.test/search?q=%D0%BA%D0%BE%D1%82+dog%3B"))
        return 51;
    {
        static const char bad_submit[] =
            "<input type=\"submit\" name=\"submit\" class=\"search\">";
        static const char good_search[] =
            "<input type=\"text\" name=\"q\" class=\"search\">";
        if (probable_search_input(bad_submit,
                                  bad_submit + strlen(bad_submit) - 1,
                                  field, sizeof(field))) return 47;
        if (!probable_search_input(good_search,
                                   good_search + strlen(good_search) - 1,
                                   field, sizeof(field)) || strcmp(field, "q")) return 48;
    }
    generic = rewrite_html(generic_sample, sizeof(generic_sample) - 1,
                           "https://example.test/page", "/page/lite/", MODE_SUPERLITE,
                           &generic_length);
    if (!generic) return 11;
    if (!strstr(generic, ">Search<span>")) return 12;
    if (!strstr(generic, "%2Fposter.webp")) return 13;
    if (generic_length != strlen(generic)) return 14;
    if (!strstr(generic, "<input type=\"submit\" value=\"Search\">")) return 15;
    if (strstr(generic, "opensearchdescription")) return 16;
    if (strstr(generic, "<form><input name=\"login\"><button type=\"submit\">Search"))
        return 17;
    free(generic);
    google = rewrite_html(google_sample, sizeof(google_sample) - 1,
                          "https://www.google.com/", "/page/lite/", MODE_SUPERLITE,
                          &google_length);
    if (!google) return 18;
    if (!strstr(google, "www.google.com%2Fsearch")) return 19;
    if (!strstr(google, "name=\"q\"")) return 20;
    if (strstr(google, "</body><hr>")) return 21;
    if (google_length != strlen(google)) return 22;
    free(google);
    original = rewrite_html(mode_sample, sizeof(mode_sample) - 1,
                            "https://example.test/page", "/page/go/", MODE_ORIGINAL,
                            &mode_length);
    if (!original) return 23;
    if (!strstr(original, "<style>.x{color:red}</style>")) return 24;
    if (!strstr(original, "<script>bad()</script>")) return 25;
    if (!strstr(original, "onclick=\"bad()\"")) return 26;
    if (!strstr(original, "<base target=\"_top\">")) return 27;
    if (!strstr(original, "/go/https%3A%2F%2Fexample.test%2Fnext")) return 28;
    if (!strstr(original, "/page/go/https%3A%2F%2Fexample.test%2Fsite.css")) return 29;
    if (!strstr(original, "target=\"content\"")) return 30;
    free(original);
    light = rewrite_html(mode_sample, sizeof(mode_sample) - 1,
                         "https://example.test/page", "/page/light/", MODE_LIGHT,
                         &mode_length);
    if (!light) return 31;
    if (!strstr(light, "<style>.x{color:red}</style>")) return 32;
    if (!strstr(light, "/page/light/https%3A%2F%2Fexample.test%2Fsite.css")) return 33;
    if (!strstr(light, "/light/https%3A%2F%2Fexample.test%2Fnext")) return 34;
    if (!strstr(light, "/page/light/https%3A%2F%2Fexample.test%2Ffind")) return 35;
    if (strstr(light, "bad()")) return 36;
    if (!strstr(light, "<base target=\"_top\">")) return 37;
    free(light);
    superlite = rewrite_html(mode_sample, sizeof(mode_sample) - 1,
                             "https://example.test/page", "/page/lite/",
                             MODE_SUPERLITE, &mode_length);
    if (!superlite) return 38;
    if (strstr(superlite, "<style") || strstr(superlite, "stylesheet")) return 39;
    if (!strstr(superlite, "<base target=\"_top\">")) return 40;
    free(superlite);
    if (!extract_js_string(media_sample, "hls", media_value,
                           sizeof(media_value)) ||
        strcmp(media_value, "https://cdn.test/master.m3u8?a=1&b=2")) return 57;
    if (!extract_js_string(media_sample, "download", media_value,
                           sizeof(media_value)) ||
        strcmp(media_value, "https://dl.test/get?x=1&title=Movie")) return 58;
    extract_html_title(media_sample, media_title, sizeof(media_title));
    if (strcmp(media_title, "Test Movie")) return 59;
    media_catalog_test = synchroncode_adapter(media_sample,
            "https://api.synchroncode.com/embed/movie/5279", &media_id);
    if (!media_catalog_test) return 62;
    if (media_catalog_test->source_count != 2 ||
        media_catalog_test->selected_source != 0)
        return 63;
    if (media_catalog_test->source[0].type != MEDIA_SOURCE_MP4 ||
        media_catalog_test->source[1].type != MEDIA_SOURCE_HLS) return 64;
    hls_catalog_test = synchroncode_adapter(hls_only_sample,
            "https://api.synchroncode.com/embed/trailer/5279", &hls_id);
    if (!hls_catalog_test) return 65;
    if (hls_catalog_test->source_count != 1 ||
        hls_catalog_test->selected_source != -1 ||
        media_source_by_id(hls_catalog_test, -1) != NULL) return 66;
    if (media_id == hls_id || media_catalog_by_id(media_id) != media_catalog_test ||
        media_catalog_test->selected_source != 0) return 67;
    if (hls_master_add_variants(media_catalog_test, hls_master_sample,
                                "https://cdn.test/master.m3u8") != 3) return 73;
    if (media_catalog_test->source[media_catalog_test->selected_source].quality != 360 ||
        media_catalog_test->source[media_catalog_test->selected_source].hls_kind != HLS_KIND_MUXED ||
        strcmp(media_catalog_test->source[media_catalog_test->selected_source].url,
               "https://cdn.test/360/index.m3u8")) return 74;
    if (strcmp(media_catalog_test->source[2].audio_url,
               "https://cdn.test/audio/index.m3u8")) return 88;
    {
        media_catalog selection_test;
        memset(&selection_test, 0, sizeof(selection_test));
        selection_test.selected_source = -1;
        source_index = media_catalog_add(&selection_test, MEDIA_SOURCE_HLS, 720,
                                  "720p", "https://cdn.test/720.m3u8");
        selection_test.source[source_index].hls_kind = HLS_KIND_MUXED;
        source_index = media_catalog_add(&selection_test, MEDIA_SOURCE_HLS, 480,
                                  "480p", "https://cdn.test/480.m3u8");
        selection_test.source[source_index].hls_kind = HLS_KIND_MUXED;
        media_catalog_select_360(&selection_test);
        if (selection_test.source[selection_test.selected_source].quality != 480)
            return 76;
        memset(&selection_test, 0, sizeof(selection_test));
        selection_test.selected_source = -1;
        source_index = media_catalog_add(&selection_test, MEDIA_SOURCE_HLS, 240,
                                  "240p", "https://cdn.test/240.m3u8");
        selection_test.source[source_index].hls_kind = HLS_KIND_MUXED;
        source_index = media_catalog_add(&selection_test, MEDIA_SOURCE_HLS, 144,
                                  "144p", "https://cdn.test/144.m3u8");
        selection_test.source[source_index].hls_kind = HLS_KIND_MUXED;
        media_catalog_select_360(&selection_test);
        if (selection_test.source[selection_test.selected_source].quality != 240)
            return 77;
        memset(&selection_test, 0, sizeof(selection_test));
        selection_test.selected_source = -1;
        source_index = media_catalog_add(&selection_test, MEDIA_SOURCE_HLS, 360,
                                  "360p VIDEO ONLY", "https://cdn.test/v360.m3u8");
        selection_test.source[source_index].hls_kind = HLS_KIND_VIDEO_ONLY;
        media_catalog_select_360(&selection_test);
        if (selection_test.selected_source != -1) return 87;
    }
    light = rewrite_html(media_sample, sizeof(media_sample) - 1,
                         "https://movies.test/page", "/page/light/", MODE_LIGHT,
                         &mode_length);
    if (!light) return 60;
    if (!strstr(light, "Open video controls") ||
        !strstr(light, "api.synchroncode.com%2Fembed%2Fmovie%2F5279")) return 61;
    free(light);
    light = rewrite_html(iframe_sample, sizeof(iframe_sample) - 1,
                         "https://movies.test/page", "/page/light/", MODE_LIGHT,
                         &mode_length);
    if (!light) return 68;
    if (!strstr(light, "Video iframe: Kodik") ||
        !strstr(light, "kodikplayer.com%2Fserial%2F1%2Fhash%2F720p")) return 69;
    if (!strstr(light, "Video iframe: Ceramet") ||
        !strstr(light, "ceramet.net%2Fbil%2F10682")) return 70;
    if (!strstr(light, "Video iframe: Unknown player") ||
        !strstr(light, "mystery.test%2Fembed%2Ffilm%2F7")) return 71;
    if (strstr(light, "widgets.test%2Fweather")) return 72;
    free(light);
    if (!iframe_referer_for("https://kodikplayer.com/serial/1/hash/720p",
                            mapped_referer, sizeof(mapped_referer)) ||
        strcmp(mapped_referer, "https://movies.test/page")) return 75;
    hls_local = rewrite_hls_playlist(hls_media_sample,
                    sizeof(hls_media_sample) - 1,
                    "https://cdn.test/video/360/index.m3u8?token=one", 2, 4,
                    &hls_local_length);
    if (!hls_local || hls_local_length != strlen(hls_local)) return 78;
    if (!strstr(hls_local, "http://127.0.0.1:8080/media/hls-resource.mp4?id=2&source=4&url=")) return 79;
    if (!strstr(hls_local, "https%3A%2F%2Fcdn.test%2Fvideo%2F360%2Finit.mp4"))
        return 80;
    if (!strstr(hls_local, "https%3A%2F%2Fkeys.test%2Fkey.bin")) return 81;
    if (!strstr(hls_local, "http://127.0.0.1:8080/media/hls-resource.m3u8?id=2&source=4&url="))
        return 84;
    if (!strstr(hls_local, "http://127.0.0.1:8080/media/hls-resource.key?id=2&source=4&url="))
        return 85;
    if (!strstr(hls_local, "http://127.0.0.1:8080/media/hls-resource.m4s?id=2&source=4&url="))
        return 86;
    if (!strstr(hls_local, "https%3A%2F%2Fcdn.test%2Fvideo%2F360%2Fseg-001.m4s"))
        return 82;
    if (!strstr(hls_local, "&ref=https%3A%2F%2Fcdn.test%2Fvideo%2F360%2Findex.m3u8%3Ftoken%3Done"))
        return 83;
    free(hls_local);
    puts(result);
    free(result);
    return 0;
}
