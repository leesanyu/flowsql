// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <gssapi/gssapi_ext.h>
#include <gssapi/gssapi_krb5.h>
#include <pthread.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Case {
    const char* cache;
    const char* principal;
    const char* keytab;
};

static gss_name_t Name(const char* text, gss_OID type) {
    OM_uint32 minor;
    gss_buffer_desc buffer = {strlen(text), (void*)text};
    gss_name_t name = GSS_C_NO_NAME;
    assert(gss_import_name(&minor, &buffer, type, &name) == GSS_S_COMPLETE);
    return name;
}

static gss_cred_id_t Credential(const char* key, const char* value, const char* principal, int usage) {
    OM_uint32 minor;
    gss_name_t name = Name(principal, GSS_KRB5_NT_PRINCIPAL_NAME);
    gss_key_value_element_desc item = {key, value};
    gss_key_value_set_desc store = {1, &item};
    gss_cred_id_t credential = GSS_C_NO_CREDENTIAL;
    assert(gss_acquire_cred_from(&minor, name, 0, GSS_C_NO_OID_SET, usage, &store, &credential, NULL, NULL) ==
           GSS_S_COMPLETE);
    gss_release_name(&minor, &name);
    return credential;
}

static void* Verify(void* pointer) {
    struct Case* test = pointer;
    for (int repetition = 0; repetition < 20; ++repetition) {
        OM_uint32 minor, client_rc = GSS_S_CONTINUE_NEEDED, server_rc = GSS_S_CONTINUE_NEEDED;
        gss_cred_id_t client = Credential("ccache", test->cache, test->principal, GSS_C_INITIATE);
        gss_cred_id_t server = Credential("keytab", test->keytab, "kafka/localhost@FLOWSQL.TEST", GSS_C_ACCEPT);
        gss_ctx_id_t client_context = GSS_C_NO_CONTEXT, server_context = GSS_C_NO_CONTEXT;
        gss_name_t target = Name("kafka@localhost", GSS_C_NT_HOSTBASED_SERVICE), peer = GSS_C_NO_NAME;
        gss_buffer_desc to_client = GSS_C_EMPTY_BUFFER, to_server = GSS_C_EMPTY_BUFFER;
        for (int round = 0; round < 8 && (client_rc || server_rc); ++round) {
            client_rc = gss_init_sec_context(&minor, client, &client_context, target, GSS_C_NO_OID,
                                            GSS_C_MUTUAL_FLAG, 0, GSS_C_NO_CHANNEL_BINDINGS, &to_client, NULL,
                                            &to_server, NULL, NULL);
            gss_release_buffer(&minor, &to_client);
            assert(client_rc == GSS_S_COMPLETE || client_rc == GSS_S_CONTINUE_NEEDED);
            if (to_server.length) {
                server_rc = gss_accept_sec_context(&minor, &server_context, server, &to_server,
                                                   GSS_C_NO_CHANNEL_BINDINGS, &peer, NULL, &to_client,
                                                   NULL, NULL, NULL);
                assert(server_rc == GSS_S_COMPLETE || server_rc == GSS_S_CONTINUE_NEEDED);
                gss_release_buffer(&minor, &to_server);
            }
        }
        assert(client_rc == GSS_S_COMPLETE && server_rc == GSS_S_COMPLETE);
        gss_buffer_desc displayed = GSS_C_EMPTY_BUFFER;
        assert(gss_display_name(&minor, peer, &displayed, NULL) == GSS_S_COMPLETE);
        assert(displayed.length == strlen(test->principal));
        assert(memcmp(displayed.value, test->principal, displayed.length) == 0);
        gss_release_buffer(&minor, &displayed);
        gss_release_buffer(&minor, &to_client);
        gss_release_name(&minor, &peer);
        gss_release_name(&minor, &target);
        gss_delete_sec_context(&minor, &client_context, GSS_C_NO_BUFFER);
        gss_delete_sec_context(&minor, &server_context, GSS_C_NO_BUFFER);
        gss_release_cred(&minor, &client);
        gss_release_cred(&minor, &server);
    }
    return NULL;
}

int main(int argc, char** argv) {
    assert(argc == 4);
    struct Case cases[] = {{argv[1], "alice@FLOWSQL.TEST", argv[3]}, {argv[2], "bob@FLOWSQL.TEST", argv[3]}};
    const char* old_cache = getenv("KRB5CCNAME");
    char* retained = old_cache ? strdup(old_cache) : NULL;
    pthread_t threads[2];
    for (int i = 0; i < 2; ++i) assert(pthread_create(&threads[i], NULL, Verify, &cases[i]) == 0);
    for (int i = 0; i < 2; ++i) assert(pthread_join(threads[i], NULL) == 0);
    old_cache = getenv("KRB5CCNAME");
    assert((!retained && !old_cache) || (retained && old_cache && strcmp(retained, old_cache) == 0));
    free(retained);
    puts("PASS: two private principals, 40 parallel GSSAPI handshakes, unchanged global credential cache");
    return 0;
}
