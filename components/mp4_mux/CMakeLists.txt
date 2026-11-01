# MP4 container built from the raw H.264 and AAC streams of a video download.
# Pure logic; compiled on the target only where something uses it.
idf_component_register(SRCS "mp4_mux.cpp"
                       INCLUDE_DIRS ".")
