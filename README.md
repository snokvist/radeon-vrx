apt packages (Ubuntu)

Build toolchain and development headers: install build-essential pkg-config libgtk-4-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libglib2.0-dev so the Makefile can invoke GCC/pkg-config and the code can include GTK, GStreamer, and GLib headers.

Runtime GStreamer plugins: install gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-libav gstreamer1.0-vaapi gstreamer1.0-gtk4 to provide the RTP H.265 depayloader/parser, VA-API and libav decoders, GTK/Wayland/video sinks, videorate, and audio converters used by the pipeline.

Testing tools: install gstreamer1.0-tools if you plan to follow the documented gst-launch-1.0 test stream workflow.
