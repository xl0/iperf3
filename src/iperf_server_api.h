
int param_received(struct iperf_stream *sp, struct param_exchange * param);
void send_result_to_client(struct iperf_stream * sp);
void iperf_run_server(struct iperf_test * test);
struct iperf_stream *find_stream_by_socket(struct iperf_test * test, int sock);


