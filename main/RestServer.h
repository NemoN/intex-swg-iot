#ifndef RestServer_h
#define RestServer_h

#ifdef __cplusplus
extern "C" {
#endif

    void systemRebootTask(void * parameter);
    void start_rest_server();
    void register_server_uri_handlers();

#ifdef __cplusplus
}
#endif

#endif // RestServer_h