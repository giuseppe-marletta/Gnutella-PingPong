#include <fstream>
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/ipv4-address.h"
#include "ns3/gnuplot.h"
#include <ctime>
#include <unordered_set>
#include <random>
#include <queue>
#include <sstream> 
#include "ns3/netanim-module.h"



using namespace ns3;
using namespace std;


NS_LOG_COMPONENT_DEFINE("GnutellaApp");

enum MessageType {
    PING = 0,
    PONG = 1,
    FORWARDPING = 2,
    FORWARDPONG = 3
};

struct Message {
    MessageType type;
    uint32_t IdReceiver;
    Ptr<Packet> packet;
};
    
uint32_t totalPacketsSent = 0;

class GnutellaHeader : public Header {
    public:
        int ttl;
        int hops;
        int descriptorType;
        int payloadLength;
        array<uint8_t,16> descriptorId;

        static TypeId GetTypeId() {
            static TypeId tid = TypeId("ns3::GnutellaHeader")
                .SetParent<Header>()
                .SetGroupName("Applications");
            return tid;
        }

        TypeId GetInstanceTypeId() const override {
            return GetTypeId();
        }

        void Serialize(Buffer::Iterator start) const override {
            for(const auto& id : descriptorId) start.WriteU8(id);
            start.WriteU8(ttl);
            start.WriteU8(hops);
            start.WriteU8(descriptorType);
            start.WriteU8(payloadLength);
        }

        uint32_t Deserialize(Buffer::Iterator start) override {
            for(auto &id : descriptorId) id = start.ReadU8();
            ttl = start.ReadU8();
            hops = start.ReadU8();
            descriptorType = start.ReadU8();
            payloadLength = start.ReadU8();
            return GetSerializedSize();
        }


        uint32_t GetSerializedSize() const override {
            return 16+1+1+1+4;
        }

        void Print(ostream &os) const override {
            os << "DescriptorID: ";
            for(const auto &id : descriptorId) os << hex << static_cast<int>(id) << " ";
            os << dec << ", Type: " << descriptorType
               << ", TTL: " << ttl 
               << ", Hops: " << hops
               << ", PayloadLength: " << payloadLength;
        }
};



class PongPayload : public Header {
    public:
        int port;
        Ipv4Address ipAddress;
        int sharedFiles;
        int sharedKB;

        static TypeId GetTypeId() {
            static TypeId tid = TypeId("ns3::PongPayload")
                .SetParent<Header>()
                .SetGroupName("Applications");
            return tid;
        }

        TypeId GetInstanceTypeId() const override {
            return GetTypeId();
        }

        void Serialize(Buffer::Iterator start) const override {
            start.WriteU16(port);
            WriteTo(start, ipAddress);
            start.WriteU32(sharedFiles);
            start.WriteU32(sharedKB);
        }

        uint32_t Deserialize(Buffer::Iterator start) override {
            port = start.ReadU16();
            uint32_t rawIp = start.ReadNtohU32();
            ipAddress = Ipv4Address(rawIp);
            sharedFiles = start.ReadU32();
            sharedKB = start.ReadU32();
            return GetSerializedSize();
        }

        uint32_t GetSerializedSize() const override {
            return 2+4+4+4;
        }

        void Print(ostream &os) const override {
            os << "Port: " << port << ", IP: " << ipAddress
               << ", SharedFiles: " << sharedFiles
               << ", SharedKB: " << sharedKB;
        }
};



class GnutellaApp : public Application {
    private:
        Ptr<Socket> m_socket;
        unordered_map<uint32_t, Ptr<Socket>> m_neighborSockets;
        Address m_peer;
        uint16_t m_port;
        uint16_t Nttl;
        queue<Message> m_messageQueue;
        uint32_t m_messageQueueSize;
        unordered_set<string> m_seenMessages;
        unordered_map<string, Ipv4Address> m_ipToSeenMessages;
        vector<uint32_t> m_neighbors;
        unordered_map<uint32_t, unordered_map<u_int32_t, Ipv4Address>> m_routingTable;
        unordered_map<uint32_t, uint32_t> m_ipToNode;
        unordered_map<string, Ipv4Address> PingOrigins;
        bool isRunning = true;
        uint32_t m_packetsSent = 0;
        


        void SendPing() {
                
            NS_LOG_INFO("Nodo " << GetNode()->GetId() << " sta inviando un ping.");
            GnutellaHeader header;
            header.ttl = Nttl;
            header.hops = 0;
            header.descriptorType = 0x01;
            header.payloadLength = 0;

            uint32_t myNodeId = GetNode()->GetId();


            random_device rd;
            generate(header.descriptorId.begin(), header.descriptorId.end(), [&rd]() { return rd();});

            ostringstream oss;
            for(const auto& byte : header.descriptorId)
                oss << static_cast<int>(byte) << " ";

            string idReadble = oss.str();

            m_seenMessages.insert(idReadble);
            


            Ptr<Packet> packet = Create<Packet>();
            packet->AddHeader(header);

    

            for(uint32_t neighborId : m_neighbors) {
                if(m_messageQueue.size() >= m_messageQueueSize)
                {
                    NS_LOG_INFO("Coda messaggi piena, ping creato da " << GetNode()->GetId() << " per il nodo " << neighborId << " scartato.");
                    continue;
                }
                Message msg = {MessageType::PING, neighborId, packet};
                m_messageQueue.push(msg);
            }

        }

        void HandleRead(Ptr<Socket> socket) {
            Ptr<Packet> packet;
            Address from;
            while((packet = socket->RecvFrom(from))) {
                GnutellaHeader header;
                packet->RemoveHeader(header);

                PongPayload payload;
                if(header.descriptorType == 0x02)
                    packet->RemoveHeader(payload);

                Ipv4Address senderAddress = InetSocketAddress::ConvertFrom(from).GetIpv4();

                NS_LOG_INFO("Nodo " << GetNode()->GetId() << " ha ricevuto un pacchetto " << (header.descriptorType == 0x01? "ping " : "pong " ) << "dal nodo " << m_ipToNode[senderAddress.Get()]);

                ostringstream oss;
                for(const auto& byte : header.descriptorId)
                    oss << static_cast<int>(byte) << " ";

                string idReadble = oss.str();


                if(m_seenMessages.find(idReadble) != m_seenMessages.end() && header.descriptorType == 0x01) {     //se si riceve un ping con un id già visto 
                    NS_LOG_INFO("Messaggio arrivato dal nodo " << m_ipToNode[senderAddress.Get()] << " ma già ricevuto, ignorato: " << idReadble);
                    continue;
                } else if (m_seenMessages.find(idReadble) == m_seenMessages.end() && header.descriptorType == 0x02) {    //se si riceve un pong con un id mai visto(mai ricevuto il rispettivo ping)
                    NS_LOG_INFO("Messaggio pong arrivato dal nodo " << m_ipToNode[senderAddress.Get()] << " ma ping mai ricevuto, scartato: " << idReadble);
                    continue;
                } else if (m_seenMessages.find(idReadble) == m_seenMessages.end() && header.descriptorType == 0x01 ) {   //se si riceve un ping con un id mai visto
                    m_seenMessages.insert(idReadble);
                    m_ipToSeenMessages[idReadble] = senderAddress;
                }  

                header.hops ++;
                header.ttl --;

                if(header.ttl == 0 && header.descriptorType == 0x01) {
                    NS_LOG_INFO("Messaggio ping arrivato dal nodo " << m_ipToNode[senderAddress.Get()] << " ma scartato (TTL=0): " << idReadble);
                    continue;
                } else if ( header.ttl <= 0 && header.descriptorType == 0x02) {
                    NS_LOG_INFO("Messaggio pong arrivato dal nodo " << m_ipToNode[senderAddress.Get()] << " ma scartato (TTL=0): " << idReadble);
                    continue;
                }


                uint32_t myNodeId = GetNode()->GetId();
                    
                Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
                uv->SetAttribute("Min", DoubleValue(0.1));
                uv->SetAttribute("Max", DoubleValue(0.3));
                if(header.descriptorType == 0x01) {
                    Simulator::Schedule(Seconds(uv->GetValue()), &GnutellaApp::ForwardPing, this, senderAddress, myNodeId, header);
                    uv->SetAttribute("Min", DoubleValue(0.1));
                    uv->SetAttribute("Max", DoubleValue(0.6));
                    Simulator::Schedule(Seconds(uv->GetValue()), &GnutellaApp::SendPong, this, header, from, myNodeId);
                }
                else if (header.descriptorType == 0x02) {
                        NS_LOG_INFO("Nodo " << GetNode()->GetId() << " ha ricevuto pong dal nodo " << m_ipToNode[payload.ipAddress.Get()] << " che ha condiviso " << payload.sharedFiles << " files (" << payload.sharedKB << " KB)");
                        Simulator::Schedule(Seconds(uv->GetValue()), &GnutellaApp::ForwardPong, this, idReadble, header, payload);
                }

            }
        }


        void ForwardPing(Ipv4Address senderAddress, uint32_t myNodeId, GnutellaHeader header) {
            
            Ptr<Packet> forwardPingPacket = Create<Packet>();
            forwardPingPacket->AddHeader(header);

            for(uint32_t neighborId : m_neighbors) {
                Ipv4Address  neighborAddress = m_routingTable[neighborId][myNodeId];
                    if(neighborAddress == senderAddress) {
                        continue;
                    }
                if(m_messageQueue.size() >= m_messageQueueSize)
                {
                    NS_LOG_INFO("Coda messaggi piena, ping ricevuto da " << m_ipToNode[senderAddress.Get()] << " per il nodo " << GetNode()->GetId() << " da inoltrare al nodo " << m_ipToNode[neighborAddress.Get()] << " scartato.");
                    continue;
                }

                Message msg = {MessageType::FORWARDPING, neighborId, forwardPingPacket};
                m_messageQueue.push(msg);
            }
        }


        void SendPong(GnutellaHeader header, Address from, uint32_t myNodeId) {
            PongPayload pong;
            pong.port = 5000;

            Ipv4Address fromAddress = InetSocketAddress::ConvertFrom(from).GetIpv4();
            Ipv4Address localIp = m_routingTable[myNodeId][m_ipToNode[fromAddress.Get()]];
            

            if(localIp == Ipv4Address::GetAny()) {
                NS_LOG_WARN("Impossibile determinare l'indirizzo IP locale per il Pong.");
                return;
            }

            pong.ipAddress = localIp;
            pong.sharedFiles = 10;
            pong.sharedKB = 2048;

            header.descriptorType = 0x02;
            header.ttl = Nttl;
            header.hops = 0;

            Ptr<Packet> pongPacket = Create<Packet>();
            pongPacket->AddHeader(pong);
            pongPacket->AddHeader(header);

            if(m_messageQueue.size() >= m_messageQueueSize)
            {
                NS_LOG_INFO("Coda messaggi piena, pong creato da " << GetNode()->GetId() << " per il nodo " << m_ipToNode[fromAddress.Get()] << " scartato.");
                return;
            }
            Message msg = {MessageType::PONG, m_ipToNode[fromAddress.Get()], pongPacket};
            m_messageQueue.push(msg);
              
        }

        void ForwardPong(string idReadble, GnutellaHeader header, PongPayload payload)
        {
            Ipv4Address ipPongForward;
            ipPongForward = m_ipToSeenMessages[idReadble];
            if(ipPongForward == Ipv4Address("102.102.102.102" )) return;
            Ptr<Packet> forwardPongPacket = Create<Packet>();
            forwardPongPacket->AddHeader(payload);
            forwardPongPacket->AddHeader(header);
            if(m_messageQueue.size() >= m_messageQueueSize)
            {
                NS_LOG_INFO("Coda messaggi piena, pong inoltrato da " << GetNode()->GetId() << " per il nodo " << m_ipToNode[ipPongForward.Get()] << " scartato.");
                return;
            }
            Message msg = {MessageType::FORWARDPONG, m_ipToNode[ipPongForward.Get()], forwardPongPacket};
            m_messageQueue.push(msg);
        }

        void SendPacket()
        {
            if(!m_messageQueue.empty())
            {
                Message msg = m_messageQueue.front();
                m_messageQueue.pop();
                if(msg.type == MessageType::PING)
                {
                    int nBytePackets = m_neighborSockets[msg.IdReceiver]->Send(msg.packet);
                    if(nBytePackets > 0)
                    {
                        totalPacketsSent++;
                        m_packetsSent++;
                        NS_LOG_INFO("Nodo " << GetNode()->GetId() << " ha inviato ping al nodo " << msg.IdReceiver);
                    }
                    else NS_LOG_INFO("Nodo " << GetNode()->GetId() << " non è riuscito ad inviare il ping al nodo " << msg.IdReceiver);
                }
                else if(msg.type == MessageType::PONG)
                {
                    int nBytePackets = m_neighborSockets[msg.IdReceiver]->Send(msg.packet);
                    if(nBytePackets > 0)
                    {
                        totalPacketsSent++;
                        m_packetsSent++;
                        NS_LOG_INFO("Nodo " << GetNode()->GetId() << " ha inviato pong al nodo " << msg.IdReceiver);
                    }
                    else NS_LOG_INFO("Nodo " << GetNode()->GetId() << " non è riuscito ad inviare il pong al nodo " << msg.IdReceiver);
                }
                else if(msg.type == MessageType::FORWARDPING)
                {
                    int nBytePackets = m_neighborSockets[msg.IdReceiver]->Send(msg.packet);
                    if(nBytePackets > 0)
                    {
                        totalPacketsSent++;
                        m_packetsSent++;
                        NS_LOG_INFO("Nodo " << GetNode()->GetId() << " ha inoltrato ping al nodo " << msg.IdReceiver);
                    }
                    else NS_LOG_INFO("Nodo " << GetNode()->GetId() << " non è riuscito ad inoltrare il ping al nodo " << msg.IdReceiver);
                }
                else if(msg.type == MessageType::FORWARDPONG)
                {
                    int nBytePackets = m_neighborSockets[msg.IdReceiver]->Send(msg.packet);
                    if(nBytePackets > 0)
                    {
                        totalPacketsSent++;
                        m_packetsSent++;
                        NS_LOG_INFO("Nodo " << GetNode()->GetId() << " ha inoltrato pong al nodo " << msg.IdReceiver);
                    }
                    else NS_LOG_INFO("Nodo " << GetNode()->GetId() << " non è riuscito ad inoltrare il pong al nodo " << msg.IdReceiver);
                }
            }
            if(isRunning)
                Simulator::Schedule(Seconds(0.5), &GnutellaApp::SendPacket, this);
        }


    public:

        void StartApplication() override {
            NS_LOG_INFO("GnutellaApp avviata sul nodo: " << GetNode()->GetId() << ", porta: " << 5000);
            m_socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
            NS_LOG_INFO("Socket creato sul nodo " << GetNode()->GetId());
            InetSocketAddress local = InetSocketAddress(Ipv4Address::GetAny(), 5000);
            m_socket->Bind(local);
            m_socket->Listen();
            m_socket->SetAcceptCallback(MakeNullCallback<bool, Ptr<Socket>, const Address&>(), MakeCallback(&GnutellaApp::ConnectionAccepted, this));
            
            for (uint32_t neighborId : m_neighbors) {
                Ptr<Socket> neighborSocket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
                Ipv4Address neighborAddress = m_routingTable[neighborId][GetNode()->GetId()];
                neighborSocket->Connect(InetSocketAddress(neighborAddress,5000));
                m_neighborSockets[neighborId] = neighborSocket;
            } 

            Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
            uv->SetAttribute("Min", DoubleValue(1.0));
            uv->SetAttribute("Max", DoubleValue(2.0));
            Simulator::Schedule(Seconds(uv->GetValue()), &GnutellaApp::SendPing, this);
            Simulator::Schedule(Seconds(uv->GetValue()+0.5), &GnutellaApp::SendPacket, this);

        }

        void ConnectionAccepted(Ptr<Socket> socket, const Address &address) {
            socket->SetRecvCallback(MakeCallback(&GnutellaApp::HandleRead, this)); 
        }

        void StopApplication() override {
            isRunning = false;
            if(m_socket)
                m_socket->Close();
            for(auto &entry : m_neighborSockets)
                entry.second->Close(); 
        }

        void SetPort(uint16_t port) { m_port = port; }

        void SetNeighbors(const vector<uint32_t>& neighbors) {
            m_neighbors = neighbors;
        }

        void SetRoutingTable(unordered_map<uint32_t, unordered_map<u_int32_t, Ipv4Address>> routingTable)
        {
            m_routingTable = routingTable;
        }

        void SetIpToNode(unordered_map<uint32_t, uint32_t> ipToNode)
        {
            m_ipToNode = ipToNode;
        }

        void SetNttl(uint16_t Nttl) { this->Nttl = Nttl; }

        void SetMessageQueueSize(uint32_t messageQueueSize) { m_messageQueueSize = messageQueueSize; }
};


int main (int argc, char *argv[])
{
    u_int32_t numNodes = 100;
    u_int16_t Nttl = 5;
    u_int32_t messageQueueSize = 500;
    u_int32_t Nlink = 4;

    CommandLine cmd;
    cmd.AddValue("numNodes", "Numero di nodi", numNodes);
    cmd.AddValue("Nttl", "Valore TTL", Nttl);
    cmd.AddValue("messageQueueSize", "Dimensione coda messaggi", messageQueueSize);
    cmd.AddValue("Nlink", "Numero di link per nodo", Nlink);
    cmd.Parse (argc,argv);

    LogComponentEnable("GnutellaApp", LOG_LEVEL_INFO);
    LogComponentEnable("Ipv4GlobalRouting", LOG_LEVEL_INFO);

    RngSeedManager::SetSeed(time(0));

    NodeContainer nodes;
    nodes.Create(numNodes);

    PointToPointHelper pointToPoint;
    pointToPoint.SetDeviceAttribute("DataRate", StringValue("5Mbps"));
    pointToPoint.SetChannelAttribute("Delay", StringValue("2ms"));

    vector<NetDeviceContainer> deviceContainers;
    
    InternetStackHelper stack;
    stack.Install(nodes);

    int countLink[numNodes]; 
    for ( int i = 0; i < numNodes; i++)
        countLink[i] = 0;

    unordered_map<uint32_t, unordered_map<u_int32_t, Ipv4Address>> routingTable;
    unordered_map<uint32_t, uint32_t> ipToNode;

    for (uint32_t i = 0; i < numNodes; i++) {
        for (int j = 0; j < Nlink; j++) {
            int maxAttempts = 100;
            int attempts = 0;
            bool found = false;

            if (countLink[i] < Nlink ) {
                Ptr<Node> neighborNode;
                uint32_t neighbor;
                do {
                    Ptr<UniformRandomVariable> rand = CreateObject<UniformRandomVariable>();
                    rand->SetAttribute("Min", DoubleValue(0));
                    rand->SetAttribute("Max", DoubleValue(numNodes-1));
                    neighbor = rand->GetInteger();
                    neighborNode = nodes.Get(neighbor);
                    if(neighbor != i && 
                        count_if(deviceContainers.begin(), deviceContainers.end(),[&](NetDeviceContainer &c) {
                            return (c.Get(0)->GetNode() == nodes.Get(i) && c.Get(1)->GetNode() == nodes.Get(neighbor)) ||
                                (c.Get(0)->GetNode() == nodes.Get(neighbor) && c.Get(1)->GetNode() == nodes.Get(i));
                        }) == 0 && countLink[neighbor] < Nlink) {
                        found = true;
                        break;   
                    }
                    attempts++;  
                } while (attempts < maxAttempts); 


                if(!found) {
                    cout << "Il nodo " << i << " non può più collegarsi con altri nodi" << endl;
                    break;
                } 

                countLink[i]++;
                countLink[neighbor]++;
                NetDeviceContainer link = pointToPoint.Install(nodes.Get(i), nodes.Get(neighbor));
                deviceContainers.push_back(link);

                Ipv4AddressHelper address;
                ostringstream subnet;
                subnet << "10." << (i / 256 ) << "." << (i % 256) << "." << ( j << 4);
                address.SetBase(subnet.str().c_str(), "255.255.255.240");
                Ipv4InterfaceContainer interfaces = address.Assign(link); 

                routingTable[i][neighbor] = interfaces.GetAddress(0);
                routingTable[neighbor][i] = interfaces.GetAddress(1);

                ipToNode[interfaces.GetAddress(0).Get()] = i;
                ipToNode[interfaces.GetAddress(1).Get()] = neighbor;

                cout << "nodo " << i << " collegato con nodo " << neighborNode->GetId() << " con IPs: " << interfaces.GetAddress(0) << " - " << interfaces.GetAddress(1) << endl ;
            }
            else break;
        }
    }

    unordered_map<uint32_t, vector<uint32_t>> nodeLinks;
    for(const auto& link : deviceContainers) {
        uint32_t nodeA = link.Get(0)->GetNode()->GetId();
        uint32_t nodeB = link.Get(1)->GetNode()->GetId();

        nodeLinks[nodeA].push_back(nodeB);
        nodeLinks[nodeB].push_back(nodeA);
    }

    ApplicationContainer apps;
    for(int i  = 0; i < numNodes; i++) {
        Ptr<GnutellaApp> app = CreateObject<GnutellaApp>();
        app->SetPort(5001+i);
        app->SetNeighbors(nodeLinks[i]);
        app->SetRoutingTable(routingTable);
        app->SetIpToNode(ipToNode);
        app->SetNttl(Nttl);
        app->SetMessageQueueSize(messageQueueSize);
        nodes.Get(i)->AddApplication(app);
        apps.Add(app);
    }

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();


    apps.Start(Seconds(1.0));
    apps.Stop(Seconds(10000.0));

    Simulator::Schedule(Seconds(10000.1), [Nlink, Nttl]() {
        ofstream outfile;
        outfile.open("traffic_data.txt", ios::app);
        outfile << totalPacketsSent << " " << Nlink << " " << Nttl << endl;
        outfile.close();
        Gnuplot gnuplot("traffic_gnutella_plot.png");
        gnuplot.SetTitle("Traffico in funzione del numero di link e TTL");
        gnuplot.SetTerminal("png");
        gnuplot.SetLegend("Numero di link", "Numero di pacchetti inviati");
        gnuplot.AppendExtra("set xrange [3:11]");
        gnuplot.AppendExtra("set yrange [0:1200000]");

        map<uint16_t, Gnuplot2dDataset> datasets;

        ifstream infile("traffic_data.txt");
        uint32_t totalpacketsSent, numLinks;
        uint16_t ttl;
        while(infile >> totalpacketsSent >> numLinks >> ttl) {
            if(datasets.find(ttl) == datasets.end()) {
                Gnuplot2dDataset dataset;
                dataset.SetTitle("TTL=" + to_string(ttl));
                dataset.SetStyle(Gnuplot2dDataset::LINES_POINTS);
                datasets[ttl] = dataset;
            }
        datasets[ttl].Add(numLinks, totalpacketsSent);
        }
        infile.close();

        for(const auto& entry : datasets)
            gnuplot.AddDataset(entry.second);

        ofstream plotFile ("traffic_gnutella_plot.plt");

        gnuplot.GenerateOutput(plotFile);

        plotFile.close();
    });
   

    Simulator::Run();
    Simulator::Destroy();

    system("gnuplot traffic_gnutella_plot.plt");


    return 0;
}
