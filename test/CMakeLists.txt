# conformance + spike test targets land with the spike task
addtest(cid_spike_test cid_spike_test.cpp)
target_link_libraries(cid_spike_test
    sgns-blockstore
    p2p::p2p_cid
)
