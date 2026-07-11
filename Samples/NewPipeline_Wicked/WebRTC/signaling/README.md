# NewPipeline_Wicked signaling relay

This is the standalone signaling relay for the real WebRTC path. It forwards
only room membership and SDP/ICE signaling; frame data never passes through it.

Run `start_signaling.sh` on macOS/Linux or `start_signaling.cmd` on Windows.
The default endpoint is `ws://127.0.0.1:39876`. Set the `PORT` environment
variable before starting the script to use another port.

Each room accepts one `client` and one `server`. Start the relay first, then the
NewPipeline_Wicked Server and Client. Both applications use the default endpoint
and room without command-line arguments.
