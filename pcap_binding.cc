#include <node.h>
#include <node_events.h>
#include <node_buffer.h>
#include <node_version.h>
#include <assert.h>
#include <pcap/pcap.h>
#include <v8.h>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ev.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>

#include <vector>

using namespace v8;
using namespace node;

// this structure represents one pcap capturing session
// both online and offline
struct pcap_session_data {
  bpf_u_int32 mask;
  bpf_u_int32 net;
  struct bpf_program fp;
  pcap_t *pcap_handle;
  Local<Function> callback;
  char *buffer_data;
  size_t buffer_length;
};

// 1. we do not expect multithreaded calls currently
// 2. it is NOT expected that library will be used
//    with tons of open/closed pcap_sessions. This step from one-session
//    just allows to run a few sessions (like,
//    open several dumps from one app execution).
//    That's because sessions vector is never cleaned. In future, we must handle
//    close of the session and free the allocated memory.
std::vector<pcap_session_data> sessions;


// PacketReady is called from within pcap, still on the stack of Dispatch.  It should be called
// only one time per Dispatch, but sometimes it gets called 0 times.  PacketReady invokes the
// JS callback associated with the dispatch() call in JS.
//
// Stack:
// 1. readWatcher.callback (pcap.js)
// 2. binding.dispatch (pcap.js)
// 3. Dispatch (binding.cc)
// 4. pcap_dispatch (libpcap)
// 5. PacketReady (binding.cc)
// 6. binding.dispatch callback (pcap.js)
//

// instead of passing just callback, we'll pass pointer to the pcap_session_data
void PacketReady(u_char *data_p, const struct pcap_pkthdr* pkthdr, const u_char* packet) {
    HandleScope scope;

    pcap_session_data * data = (pcap_session_data*)data_p;

    size_t copy_len = pkthdr->caplen;
    if (copy_len > data->buffer_length) {
        copy_len = data->buffer_length;
    }
    memcpy(data->buffer_data, packet, copy_len);

    TryCatch try_catch;

    Local<Object> packet_header = Object::New();

    packet_header->Set(String::New("tv_sec"), Integer::NewFromUnsigned(pkthdr->ts.tv_sec));
    packet_header->Set(String::New("tv_usec"), Integer::NewFromUnsigned(pkthdr->ts.tv_usec));
    packet_header->Set(String::New("caplen"), Integer::NewFromUnsigned(pkthdr->caplen));
    packet_header->Set(String::New("len"), Integer::NewFromUnsigned(pkthdr->len));

    Local<Value> argv[1] = packet_header;

    (data->callback)->Call(Context::GetCurrent()->Global(), 1, argv);

    if (try_catch.HasCaught())  {
        FatalException(try_catch);
    }
}

Handle<Value>
Dispatch(const Arguments& args)
{
    HandleScope scope;

    if (args.Length() != 3) {
        return ThrowException(Exception::TypeError(String::New("Dispatch takes exactly three arguments")));
    }

    if (!Buffer::HasInstance(args[0])) {
        return ThrowException(Exception::TypeError(String::New("First argument must be a buffer")));
    }

    if (!args[1]->IsFunction()) {
        return ThrowException(Exception::TypeError(String::New("Second argument must be a function")));
    }

    // this will be session id

    if (!args[2]->IsInt32()) {
        return ThrowException(Exception::TypeError(String::New("Third argument must be an unsingned int")));
    }

    int32_t session_id = args[2]->Int32Value();

    pcap_session_data* data = &sessions[session_id];

#if NODE_VERSION_AT_LEAST(0,3,0)
    Local<Object> buffer_obj = args[0]->ToObject();
    data->buffer_data = Buffer::Data(buffer_obj);
    data->buffer_length = Buffer::Length(buffer_obj);
#else
    Buffer *buffer_obj = ObjectWrap::Unwrap<Buffer>(args[0]->ToObject());
    data->buffer_data = buffer_obj->data();
    data->buffer_length = buffer_obj->length();
#endif

    data->callback = Local<Function>::Cast(args[1]);

    int total_packets = 0;
    total_packets = pcap_dispatch(data->pcap_handle, 1, PacketReady, (u_char *)data);

    return scope.Close(Integer::NewFromUnsigned(total_packets));
}

Handle<Value>
Open(bool live, const Arguments& args)
{
    HandleScope scope;
    char errbuf[PCAP_ERRBUF_SIZE];

    if (args.Length() == 3) {
        if (!args[0]->IsString()) {
            return ThrowException(Exception::TypeError(String::New("pcap Open: args[0] must be a String")));
        }
        if (!args[1]->IsString()) {
            return ThrowException(Exception::TypeError(String::New("pcap Open: args[1] must be a String")));
        }
        if (!args[2]->IsInt32()) {
            return ThrowException(Exception::TypeError(String::New("pcap Open: args[2] must be a Number")));
        }
    } else {
        return ThrowException(Exception::TypeError(String::New("pcap Open: expecting 3 arguments")));
    }
    String::Utf8Value device(args[0]->ToString());
    String::Utf8Value filter(args[1]->ToString());
    int buffer_size = args[2]->Int32Value();

    pcap_session_data data;

    if (live) {
        if (pcap_lookupnet((char *) *device, &data.net, &data.mask, errbuf) == -1) {
            data.net = 0;
            data.mask = 0;
            fprintf(stderr, "warning: %s - this may not actually work\n", errbuf);
        }

        data.pcap_handle = pcap_create((char *) *device, errbuf);
        if (data.pcap_handle == NULL) {
            return ThrowException(Exception::Error(String::New(errbuf)));
        }

        // 64KB is the max IPv4 packet size
        if (pcap_set_snaplen(data.pcap_handle, 65535) != 0) {
            return ThrowException(Exception::Error(String::New("error setting snaplen")));
        }

        // always use promiscuous mode
        if (pcap_set_promisc(data.pcap_handle, 1) != 0) {
            return ThrowException(Exception::Error(String::New("error setting promiscuous mode")));
        }

        // Try to set buffer size.  Sometimes the OS has a lower limit that it will silently enforce.
        if (pcap_set_buffer_size(data.pcap_handle, buffer_size) != 0) {
            return ThrowException(Exception::Error(String::New("error setting buffer size")));
        }

        // set "timeout" on read, even though we are also setting nonblock below.  On Linux this is required.
        if (pcap_set_timeout(data.pcap_handle, 1000) != 0) {
            return ThrowException(Exception::Error(String::New("error setting read timeout")));
        }

        // TODO - pass in an option to enable rfmon on supported interfaces.  Sadly, it can be a disruptive
        // operation, so we can't just always try to turn it on.
        // if (pcap_set_rfmon(pcap_handle, 1) != 0) {
        //     return ThrowException(Exception::Error(String::New(pcap_geterr(pcap_handle))));
        // }

        if (pcap_activate(data.pcap_handle) != 0) {
            return ThrowException(Exception::Error(String::New(pcap_geterr(data.pcap_handle))));
        }
    } else {
        // Device is the path to the savefile
        data.pcap_handle = pcap_open_offline((char *) *device, errbuf);
        if (data.pcap_handle == NULL) {
            return ThrowException(Exception::Error(String::New(errbuf)));
        }
    }

    if (pcap_setnonblock(data.pcap_handle, 1, errbuf) == -1) {
        return ThrowException(Exception::Error(String::New(errbuf)));
    }

    // TODO - if filter is empty, don't bother with compile or set
    if (pcap_compile(data.pcap_handle, &data.fp, (char *) *filter, 1, data.net) == -1) {
        return ThrowException(Exception::Error(String::New(pcap_geterr(data.pcap_handle))));
    }

    if (pcap_setfilter(data.pcap_handle, &data.fp) == -1) {
        return ThrowException(Exception::Error(String::New(pcap_geterr(data.pcap_handle))));
    }

    // Work around buffering bug in BPF on OSX 10.6 as of May 19, 2010
    // This may result in dropped packets under load because it disables the (broken) buffer
    // http://seclists.org/tcpdump/2010/q1/110
#if defined(__APPLE_CC__) || defined(__APPLE__)
    #include <net/bpf.h>
    int fd = pcap_get_selectable_fd(data.pcap_handle);
    int v = 1;
    ioctl(fd, BIOCIMMEDIATE, &v);
    // TODO - check return value
#endif

    // Return new id of session
    Local<Value> ret = Int32::New(sessions.size());

    sessions.push_back(data);

    return scope.Close(ret);
}

Handle<Value>
LinkType(const Arguments& args)
{
    HandleScope scope;
    char errbuf[PCAP_ERRBUF_SIZE];

    if (args.Length() == 1) {
        if (!args[0]->IsInt32()) {
            return ThrowException(Exception::TypeError(String::New("pcap Close: args[0] must be a Number")));
        }
    } else {
        return ThrowException(Exception::TypeError(String::New("pcap Close: expecting 1 argument")));
    }
    int session_id = args[0]->Int32Value();

    int link_type = pcap_datalink(sessions[session_id].pcap_handle);

    Local<Value> ret;
    switch (link_type) {
    case DLT_NULL:
        ret = String::New("LINKTYPE_NULL");
        break;
    case DLT_EN10MB: // most wifi interfaces pretend to be "ethernet"
        ret =  String::New("LINKTYPE_ETHERNET");
        break;
    case DLT_IEEE802_11_RADIO: // 802.11 "monitor mode"
        ret = String::New("LINKTYPE_IEEE802_11_RADIO");
        break;
    case DLT_RAW: // "raw IP"
        ret = String::New("LINKTYPE_RAW");
        break;
    default:
        snprintf(errbuf, PCAP_ERRBUF_SIZE, "Unknown linktype %d", link_type);
        ret = String::New(errbuf);
        break;
    }

    return scope.Close(ret);
}

Handle<Value>
OpenLive(const Arguments& args)
{
    return Open(true, args);
}

Handle<Value>
OpenOffline(const Arguments& args)
{
    return Open(false, args);
}

Handle<Value>
FindAllDevs(const Arguments& args)
{
    HandleScope scope;
    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_if_t *alldevs, *cur_dev;

    if (pcap_findalldevs(&alldevs, errbuf) == -1 || alldevs == NULL) {
        return ThrowException(Exception::TypeError(String::New(errbuf)));
    }

    Local<Array> DevsArray = Array::New();

    int i = 0;
    for (cur_dev = alldevs ; cur_dev != NULL ; cur_dev = cur_dev->next, i++) {
        Local<Object> Dev = Object::New();

        Dev->Set(String::New("name"), String::New(cur_dev->name));
        if (cur_dev->description != NULL) {
            Dev->Set(String::New("description"), String::New(cur_dev->description));
        }
        Local<Array> AddrArray = Array::New();
        int j = 0;
        for (pcap_addr_t *cur_addr = cur_dev->addresses ; cur_addr != NULL ; cur_addr = cur_addr->next, j++) {
            if (cur_addr->addr && cur_addr->addr->sa_family == AF_INET) {
                Local<Object> Address = Object::New();

                struct sockaddr_in *sin = (struct sockaddr_in *) cur_addr->addr;
                Address->Set(String::New("addr"), String::New(inet_ntoa(sin->sin_addr)));

                if (cur_addr->netmask != NULL) {
                    sin = (struct sockaddr_in *) cur_addr->netmask;
                    Address->Set(String::New("netmask"), String::New(inet_ntoa(sin->sin_addr)));
                }
                if (cur_addr->broadaddr != NULL) {
                    sin = (struct sockaddr_in *) cur_addr->broadaddr;
                    Address->Set(String::New("broadaddr"), String::New(inet_ntoa(sin->sin_addr)));
                }
                if (cur_addr->dstaddr != NULL) {
                    sin = (struct sockaddr_in *) cur_addr->dstaddr;
                    Address->Set(String::New("dstaddr"), String::New(inet_ntoa(sin->sin_addr)));
                }
                AddrArray->Set(Integer::New(j), Address);
            }
            // TODO - support AF_INET6
        }

        Dev->Set(String::New("addresses"), AddrArray);

        if (cur_dev->flags & PCAP_IF_LOOPBACK) {
            Dev->Set(String::New("flags"), String::New("PCAP_IF_LOOPBACK"));
        }

        DevsArray->Set(Integer::New(i), Dev);
    }

    pcap_freealldevs(alldevs);
    return scope.Close(DevsArray);
}

Handle<Value>
Close(const Arguments& args)
{
    if (args.Length() == 1) {
        if (!args[0]->IsInt32()) {
            return ThrowException(Exception::TypeError(String::New("pcap Close: args[0] must be a Number")));
        }
    } else {
        return ThrowException(Exception::TypeError(String::New("pcap Close: expecting 1 argument")));
    }
    int session_id = args[0]->Int32Value();
    HandleScope scope;

    pcap_close(sessions[session_id].pcap_handle);

    return Undefined();
}

Handle<Value>
Fileno(const Arguments& args)
{
    if (args.Length() == 1) {
        if (!args[0]->IsInt32()) {
            return ThrowException(Exception::TypeError(String::New("pcap Fileno: args[0] must be a Number")));
        }
    } else {
        return ThrowException(Exception::TypeError(String::New("pcap Fileno: expecting 1 argument")));
    }
    int session_id = args[0]->Int32Value();
    HandleScope scope;

    int fd = pcap_get_selectable_fd(sessions[session_id].pcap_handle);

    return scope.Close(Integer::NewFromUnsigned(fd));
}

Handle<Value>
Stats(const Arguments& args)
{
    if (args.Length() == 1) {
        if (!args[0]->IsInt32()) {
            return ThrowException(Exception::TypeError(String::New("pcap Stats: args[0] must be a Number")));
        }
    } else {
        return ThrowException(Exception::TypeError(String::New("pcap Stats: expecting 1 argument")));
    }
    int session_id = args[0]->Int32Value();

    HandleScope scope;

    struct pcap_stat ps;

    if (pcap_stats(sessions[session_id].pcap_handle, &ps) == -1) {
        return ThrowException(Exception::Error(String::New("Error in pcap_stats")));
        // TODO - use pcap_geterr to figure out what the error was
    }

    Local<Object> stats_obj = Object::New();

    stats_obj->Set(String::New("ps_recv"), Integer::NewFromUnsigned(ps.ps_recv));
    stats_obj->Set(String::New("ps_drop"), Integer::NewFromUnsigned(ps.ps_drop));
    stats_obj->Set(String::New("ps_ifdrop"), Integer::NewFromUnsigned(ps.ps_ifdrop));
    // ps_ifdrop may not be supported on this platform, but there's no good way to tell, is there?

    return scope.Close(stats_obj);
}

Handle<Value>
DefaultDevice(const Arguments& args)
{
    HandleScope scope;
    char errbuf[PCAP_ERRBUF_SIZE];

    // Look up the first device with an address, pcap_lookupdev() just returns the first non-loopback device.
    Local<Value> ret;
    pcap_if_t *alldevs, *dev;
    pcap_addr_t *addr;
    bool found = false;

    if (pcap_findalldevs(&alldevs, errbuf) == -1 || alldevs == NULL) {
        return ThrowException(Exception::Error(String::New(errbuf)));
    }

    for (dev = alldevs; dev != NULL; dev = dev->next) {
        if (dev->addresses != NULL && !(dev->flags & PCAP_IF_LOOPBACK)) {
            for (addr = dev->addresses; addr != NULL; addr = addr->next) {
                // TODO - include IPv6 addresses in DefaultDevice guess
                // if (addr->addr->sa_family == AF_INET || addr->addr->sa_family == AF_INET6) {
                if (addr->addr->sa_family == AF_INET) {
                    ret = String::New(dev->name);
                    found = true;
                    break;
                }
            }

            if (found) {
                break;
            }
        }
    }

    pcap_freealldevs(alldevs);
    return scope.Close(ret);
}

Handle<Value>
LibVersion(const Arguments &args)
{
    HandleScope scope;

    return scope.Close(String::New(pcap_lib_version()));
}

extern "C" void init (Handle<Object> target)
{
    HandleScope scope;

    target->Set(String::New("findalldevs"), FunctionTemplate::New(FindAllDevs)->GetFunction());
    target->Set(String::New("open_live"), FunctionTemplate::New(OpenLive)->GetFunction());
    target->Set(String::New("open_offline"), FunctionTemplate::New(OpenOffline)->GetFunction());
    target->Set(String::New("dispatch"), FunctionTemplate::New(Dispatch)->GetFunction());
    target->Set(String::New("fileno"), FunctionTemplate::New(Fileno)->GetFunction());
    target->Set(String::New("close"), FunctionTemplate::New(Close)->GetFunction());
    target->Set(String::New("stats"), FunctionTemplate::New(Stats)->GetFunction());
    target->Set(String::New("default_device"), FunctionTemplate::New(DefaultDevice)->GetFunction());
    target->Set(String::New("lib_version"), FunctionTemplate::New(LibVersion)->GetFunction());
    target->Set(String::New("link_type"), FunctionTemplate::New(LinkType)->GetFunction());
}
