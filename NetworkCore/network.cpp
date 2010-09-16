//
// network.cpp
//
// Copyright © Quazaa Development Team, 2009-2010.
// This file is part of QUAZAA (quazaa.sourceforge.net)
//
// Quazaa is free software; you can redistribute it
// and/or modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 3 of
// the License, or (at your option) any later version.
//
// Quazaa is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Quazaa; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//

#include "network.h"

#include "thread.h"
#include "webcache.h"
#include "hostcache.h"
#include "g2packet.h"
#include "datagrams.h"
#include <QTimer>
#include <QTime>
#include <QList>
#include "g2node.h"
#include "networkconnection.h"
#include "handshakes.h"

#include "quazaasettings.h"

#include "queryhashtable.h"
#include "queryhashmaster.h"
#include "searchmanager.h"
#include "ShareManager.h"
#include "managedsearch.h"
#include "query.h"

#include "geoiplist.h"

CNetwork Network;
CThread NetworkThread;

CNetwork::CNetwork(QObject *parent)
    :QObject(parent)
{
    m_pSecondTimer = 0;
    //m_oAddress.port = 6346;
	m_oAddress.port = quazaaSettings.Connection.Port;

    m_nHubsConnected = 0;
    m_nLeavesConnected = 0;
    m_bNeedUpdateLNI = true;
    m_nLNIWait = 60;
    m_nKHLWait = 60;
    m_tCleanRoutesNext = 60;

	m_nNextCheck = 0;
	m_nBusyPeriods = 0;
	m_nTotalPeriods = 0;

	m_bSharesReady = false;

	m_nCookie = 0;
	m_nSecsToHubBalancing = 0;
	m_tLastModeChange = 0;
	m_nMinutesAbove90 = m_nMinutesBelow50 = 0;
	m_nMinutesTrying = 0;
}
CNetwork::~CNetwork()
{
    if( m_bActive )
    {
        Disconnect();
    }
}

void CNetwork::Connect()
{
    QMutexLocker l(&m_pSection);

    qDebug() << "connect " << QThread::currentThreadId();

    if( m_bActive )
    {
        qDebug() << "Network already started";
        return;
    }

	if( quazaaSettings.Gnutella2.ClientMode < 2 )
		m_nNodeState = G2_LEAF;
	else
		m_nNodeState = G2_HUB;

    m_bActive = true;
	m_oAddress.port = quazaaSettings.Connection.Port;

	m_nNextCheck = quazaaSettings.Gnutella2.AdaptiveCheckPeriod;
	m_nBusyPeriods = 0;
	m_nTotalPeriods = 0;

    Datagrams.moveToThread(&NetworkThread);
    SearchManager.moveToThread(&NetworkThread);
	Handshakes.Listen();
    m_oRoutingTable.Clear();
	m_nSecsToHubBalancing = HUB_BALANCING_INTERVAL;
	m_tLastModeChange = time(0);
	m_nMinutesAbove90 = m_nMinutesBelow50 = 0;
	m_nMinutesTrying = 0;
	connect(&ShareManager, SIGNAL(sharesReady()), this, SLOT(OnSharesReady()), Qt::UniqueConnection);
	NetworkThread.start("Network", &m_pSection, this);

}
void CNetwork::Disconnect()
{
    QMutexLocker l(&m_pSection);

    qDebug() << "CNetwork::Disconnect() ThreadID:" << QThread::currentThreadId();

    if( m_bActive )
    {
        m_bActive = false;
        NetworkThread.exit(0);
    }

}
void CNetwork::SetupThread()
{
    qWarning("In Network Thread");
    qDebug() << QThread::currentThreadId();

    Q_ASSERT(m_pSecondTimer == 0 && m_pRateController == 0);

    m_pSecondTimer = new QTimer();
	connect(m_pSecondTimer, SIGNAL(timeout()), this, SLOT(OnSecondTimer()));
    m_pSecondTimer->start(1000);

    // Powiedzmy ze mamy lacze 2Mbit/s / 128kbit/s
	quint32 nUploadCapacity = quazaaSettings.Connection.OutSpeed;
	quint32 nDownloadCapacity = quazaaSettings.Connection.InSpeed;

    // Dla polaczen TCP w sieci 1/4 dostepnego pasma
    m_pRateController = new CRateController();
	m_pRateController->moveToThread(&NetworkThread); // should not be necessary
    m_pRateController->setObjectName("CNetwork rate controller");
	m_pRateController->SetDownloadLimit(nDownloadCapacity); // /4
	m_pRateController->SetUploadLimit(nUploadCapacity);	// /4

	Datagrams.Listen();
	Handshakes.Listen();

	m_bSharesReady = ShareManager.SharesReady();
}
void CNetwork::CleanupThread()
{
    qWarning("Stopping Network Thread");
    qDebug() << "Network ThreadID: " << QThread::currentThreadId();

    m_pSecondTimer->stop();
    delete m_pSecondTimer;
    m_pSecondTimer = 0;
    WebCache.CancelRequests();

    Datagrams.Disconnect();
    Handshakes.Disconnect();

	DisconnectAllNodes();

	while( !m_lHitsToRoute.isEmpty() )
	{
		m_lHitsToRoute.takeFirst().second->Release();
	}

	delete m_pRateController;
	m_pRateController = 0;

    moveToThread(qApp->thread());
}

void CNetwork::RemoveNode(CG2Node* pNode)
{
    if( pNode->m_nType == G2_HUB )
        m_nHubsConnected--;
    else if( pNode->m_nType == G2_LEAF )
        m_nLeavesConnected--;

    m_pRateController->RemoveSocket(pNode);

    emit NodeRemoved(pNode);
    m_lNodes.removeOne(pNode);
    m_oRoutingTable.Remove(pNode);

    //qDebug() << "List size: " << m_lNodes.size();

}

void CNetwork::OnSecondTimer()
{

	if( !m_pSection.tryLock(150) )
    {
        qWarning() << "WARNING: Network core overloaded!";
        return;
	}

    if( !m_bActive )
    {
		m_pSection.unlock();
        return;
    }

	if( m_nHubsConnected == 0 && !WebCache.isRequesting() && ( HostCache.isEmpty() || HostCache.GetConnectable() == 0 ) )
    {
        WebCache.RequestRandom();
    }

    if( m_tCleanRoutesNext > 0 )
        m_tCleanRoutesNext--;
    else
    {
		//m_oRoutingTable.Dump();
        m_oRoutingTable.ExpireOldRoutes();
        m_tCleanRoutesNext = 60;
    }

	//Datagrams.FlushSendCache();
	emit Datagrams.SendQueueUpdated();

	if( isHub() && quazaaSettings.Gnutella2.AdaptiveHub && --m_nNextCheck == 0 )
	{
		AdaptiveHubRun();
		m_nNextCheck = quazaaSettings.Gnutella2.AdaptiveCheckPeriod;
	}

	if( !QueryHashMaster.IsValid() )
		QueryHashMaster.Build();

    Maintain();

	m_nSecsToHubBalancing--;

	if( m_nSecsToHubBalancing == 0 )
	{
		m_nSecsToHubBalancing = HUB_BALANCING_INTERVAL;
		HubBalancing();
	}

    SearchManager.OnTimer();

	if( m_lHitsToRoute.size() )
		FlushHits();

    if( m_nLNIWait == 0 )
    {
        if( m_bNeedUpdateLNI )
        {
			m_nLNIWait = quazaaSettings.Gnutella2.LNIMinimumUpdate;
            m_bNeedUpdateLNI = false;

            foreach( CG2Node* pNode, m_lNodes )
            {
                if( pNode->m_nState == nsConnected )
                    pNode->SendLNI();
            }
        }
    }
    else
        m_nLNIWait--;

    if( m_nKHLWait == 0 )
    {
        HostCache.Save();
        DispatchKHL();
		m_nKHLWait = quazaaSettings.Gnutella2.KHLPeriod;
    }
    else
        m_nKHLWait--;

	m_pSection.unlock();
}

void CNetwork::DisconnectAllNodes()
{
    QListIterator<CG2Node*> it(m_lNodes);
    CG2Node* pNode = 0;
    while( it.hasNext() )
    {
        pNode = it.next();
		RemoveNode(pNode);
		pNode->Close();
		delete pNode;
    }
}
bool CNetwork::NeedMore(G2NodeType nType)
{
    if( nType == G2_HUB ) // potrzeba hubow?
    {
        if( m_nNodeState == G2_HUB ) // jesli hub
			return ( m_nHubsConnected < quazaaSettings.Gnutella2.NumPeers );
        else    // jesli leaf
			return ( m_nLeavesConnected < quazaaSettings.Gnutella2.NumHubs );
    }
    else // potrzeba leaf?
    {
        if( m_nNodeState == G2_HUB )    // jesli hub
			return ( m_nLeavesConnected < quazaaSettings.Gnutella2.NumLeafs );
    }

    return false;
}

void CNetwork::Maintain()
{
    //qDebug() << "CNetwork::Maintain";
    quint32 tNow = time(0);

	m_nCookie++;

	foreach( CG2Node* pNode, m_lNodes )
	{
		if( pNode->m_nCookie != m_nCookie )
		{
			pNode->OnTimer(tNow);
			pNode->m_nCookie = m_nCookie;
		}
	}

    quint32 nHubs = 0, nLeaves = 0, nUnknown = 0;
    quint32 nCoreHubs = 0, nCoreLeaves = 0;

	foreach( CG2Node* pNode, m_lNodes )
    {
        if( pNode->m_nState == nsConnected )
        {
            switch( pNode->m_nType )
            {
            case G2_UNKNOWN:
                nUnknown++;
                break;
            case G2_HUB:
                nHubs++;
                if( pNode->m_bG2Core )
                    nCoreHubs++;
                break;
            case G2_LEAF:
                nLeaves++;
                if( pNode->m_bG2Core )
                    nCoreLeaves++;
            }
        }
        else
        {
            nUnknown++;
        }


    }

	//qDebug("Hubs: %u, Leaves: %u, unknown: %u", nHubs, nLeaves, nUnknown);

    if( m_nHubsConnected != nHubs || m_nLeavesConnected != nLeaves )
        m_bNeedUpdateLNI = true;

    m_nHubsConnected = nHubs;
    m_nLeavesConnected = nLeaves;

    if( m_nNodeState == G2_LEAF )
    {
		if( nHubs > quazaaSettings.Gnutella2.NumHubs )
        {
            // rozlaczyc
            DropYoungest(G2_HUB, (nCoreHubs / nHubs) > 0.5);
        }
		else if( nHubs < quazaaSettings.Gnutella2.NumHubs )
        {
			qint32 nAttempt = qint32((quazaaSettings.Gnutella2.NumHubs - nHubs) * quazaaSettings.Gnutella.ConnectFactor );
            nAttempt = qMin(nAttempt, 8) - nUnknown;

            quint32 tNow = time(0);
			bool bCountry = true;
			int  nCountry = 0;

            for( ; nAttempt > 0; nAttempt-- )
            {
                // nowe polaczenie
				CHostCacheHost* pHost = HostCache.GetConnectable(tNow, (bCountry ? (quazaaSettings.Connection.PreferredCountries.size() ? quazaaSettings.Connection.PreferredCountries.at(nCountry) : GeoIP.findCountryCode(m_oAddress)) : "ZZ"));

                if( pHost )
                {
					ConnectTo(pHost->m_oAddress);
                    pHost->m_tLastConnect = tNow;
                }
                else
				{
					if( !bCountry )
					{
						break;
					}
					else
					{
						if( quazaaSettings.Connection.PreferredCountries.size() )
						{
							nCountry++;
							if( nCountry >= quazaaSettings.Connection.PreferredCountries.size() )
							{
								bCountry = false;
							}
							nAttempt++;
							continue;
						}
						bCountry = false;
						nAttempt++;
					}
				}
            }

        }
    }
    else
    {
		if( nHubs > quazaaSettings.Gnutella2.NumPeers )
        {
            // rozlaczyc hub
            DropYoungest(G2_HUB, (nCoreHubs / nHubs) > 0.5);
        }
		else if( nHubs < quazaaSettings.Gnutella2.NumPeers )
        {
			qint32 nAttempt = qint32((quazaaSettings.Gnutella2.NumPeers - nHubs) * quazaaSettings.Gnutella.ConnectFactor );
            nAttempt = qMin(nAttempt, 8) - nUnknown;

            quint32 tNow = time(0);

            for( ; nAttempt > 0; nAttempt-- )
            {
                // nowe polaczenie
                CHostCacheHost* pHost = HostCache.GetConnectable(tNow);

                if( pHost )
                {
					ConnectTo(pHost->m_oAddress);
                    pHost->m_tLastConnect = tNow;
                }
                else
                    break;
            }
        }

		if( nLeaves > quazaaSettings.Gnutella2.NumLeafs )
        {
            DropYoungest(G2_LEAF, (nCoreLeaves / nLeaves) > 0.5);
        }
    }
}

void CNetwork::DispatchKHL()
{
	if( m_lNodes.isEmpty() )
        return;

    G2Packet* pKHL = G2Packet::New("KHL");
    quint32 ts = time(0);
	pKHL->WritePacket("TS", 4)->WriteIntLE(ts);

    foreach(CG2Node* pNode, m_lNodes)
    {
        if( pNode->m_nType == G2_HUB && pNode->m_nState == nsConnected )
        {
			pKHL->WritePacket("NH", 6)->WriteHostAddress(&pNode->m_oAddress);
        }
    }

	quint32 nCount = 0;

	for( ; nCount < (quint32)quazaaSettings.Gnutella2.KHLHubCount && HostCache.size() > nCount; nCount++ )
	{
		pKHL->WritePacket("CH", 10)->WriteHostAddress(&HostCache.m_lHosts.at(nCount)->m_oAddress);
		pKHL->WriteIntLE(&HostCache.m_lHosts.at(nCount)->m_tTimestamp);
	}


    foreach(CG2Node* pNode, m_lNodes)
    {
        if( pNode->m_nState == nsConnected )
        {
			pNode->SendPacket(pKHL, false, false);
        }
    }
	pKHL->Release();
}

void CNetwork::OnNodeStateChange()
{
    QObject* pSender = QObject::sender();
    if( pSender )
        emit NodeUpdated(qobject_cast<CG2Node*>(pSender));
}

void CNetwork::OnAccept(CNetworkConnection* pConn)
{
	if( !m_pSection.tryLock(50) )
	{
		qDebug() << "Not accepting incoming connection. Network overloaded";
		pConn->Close();
		pConn->deleteLater();
		return;
	}

    CG2Node* pNew = new CG2Node();
	m_pRateController->AddSocket(pNew);
	m_lNodes.append(pNew);
    pNew->AttachTo(pConn);
	pNew->moveToThread(&NetworkThread);
	connect(pNew, SIGNAL(NodeStateChanged()), this, SLOT(OnNodeStateChange()));
    emit NodeAdded(pNew);
	m_pSection.unlock();
}

bool CNetwork::IsListening()
{
    return Handshakes.isListening() && Datagrams.isListening();
}

bool CNetwork::IsFirewalled()
{
    return Datagrams.IsFirewalled() || Handshakes.IsFirewalled();
}

void CNetwork::DropYoungest(G2NodeType nType, bool bCore)
{
    CG2Node* pNode = 0;

    for( QList<CG2Node*>::iterator i = m_lNodes.begin(); i != m_lNodes.end(); i++ )
    {
        if( (*i)->m_nState == nsConnected )
        {
            if( (*i)->m_nType == nType )
            {
                if( !bCore && (*i)->m_bG2Core )
                    continue;

                if( pNode == 0 )
                {
                    pNode = (*i);
                }
                else
                {
                    if( (*i)->m_tConnected > pNode->m_tConnected )
                        pNode = (*i);
                }
            }
        }
    }

    if( pNode )
		pNode->Close();;
}

void CNetwork::AcquireLocalAddress(QString &sHeader)
{
    IPv4_ENDPOINT hostAddr(sHeader + ":0");

    if( hostAddr.ip != 0 )
    {
        m_oAddress.ip = hostAddr.ip;
    }
}
bool CNetwork::IsConnectedTo(IPv4_ENDPOINT addr)
{
    for( int i = 0; i < m_lNodes.size(); i++ )
    {
        if( m_lNodes.at(i)->m_oAddress == addr )
            return true;
    }

    return false;
}

void CNetwork::RouteHits(QUuid &oTarget, G2Packet *pPacket)
{
	pPacket->AddRef();
	m_lHitsToRoute.append(qMakePair<QUuid, G2Packet*>(oTarget, pPacket));

	if( m_lHitsToRoute.size() > 1000 )
		FlushHits();
}
void CNetwork::FlushHits()
{
	QTime tTime;

	tTime.start();

	while( !m_lHitsToRoute.isEmpty() && tTime.elapsed() < 125 )
	{
		QPair<QUuid, G2Packet*> oHitPair = m_lHitsToRoute.takeFirst();
		RoutePacket(oHitPair.first, oHitPair.second);
		oHitPair.second->Release();
	}

	qDebug() << "Hits left to be routed: " << m_lHitsToRoute.size();
}

bool CNetwork::RoutePacket(QUuid &pTargetGUID, G2Packet *pPacket)
{
    CG2Node* pNode = 0;
    IPv4_ENDPOINT pAddr;

    if( m_oRoutingTable.Find(pTargetGUID, &pNode, &pAddr) )
    {
        if( pNode )
        {
			pNode->SendPacket(pPacket, true, false);
			qDebug() << "CNetwork::RoutePacket " << pTargetGUID.toString() << " Packet: " << pPacket->GetType() << " routed to neighbour: " << pNode->m_oAddress.toString().toAscii().constData();
            return true;
        }
        else if( pAddr.ip )
        {
            Datagrams.SendPacket(pAddr, pPacket, true);
			qDebug() << "CNetwork::RoutePacket " << pTargetGUID.toString() << " Packet: " << pPacket->GetType() << " routed to remote node: " << pNode->m_oAddress.toString().toAscii().constData();
            return true;
        }
		qDebug() << "CNetwork::RoutePacket - weird thing, should not happen...";
    }

	qDebug() << "CNetwork::RoutePacket " << pTargetGUID.toString() << " Packet: " << pPacket->GetType() << " DROPPED!";
    return false;
}
bool CNetwork::RoutePacket(G2Packet *pPacket, CG2Node *pNbr)
{
	QUuid pGUID;

	if( pPacket->GetTo(pGUID) && pGUID != quazaaSettings.Profile.GUID ) // no i adres != moj adres
    {
        CG2Node* pNode = 0;
        IPv4_ENDPOINT pAddr;

        if( m_oRoutingTable.Find(pGUID, &pNode, &pAddr) )
        {
            bool bForwardTCP = false;
            bool bForwardUDP = false;

            if( pNbr )
            {
                if( pNbr->m_nType == G2_LEAF )  // if received from leaf - can forward anywhere
                {
                    bForwardTCP = bForwardUDP = true;
                }
                else    // if received from a hub - can be forwarded to leaf
                {
                    if( pNode && pNode->m_nType == G2_LEAF )
                    {
                        bForwardTCP = true;
                    }
                }
            }
            else    // received from udp - do not forward via udp
            {
                bForwardTCP = true;
            }

            if( pNode && bForwardTCP )
            {
				pNode->SendPacket(pPacket, true, false);
                return true;
            }
            else if( pAddr.ip && bForwardUDP )
            {
                Datagrams.SendPacket(pAddr, pPacket, true);
                return true;
            }
            // drop
        }
        return true;
	}
    return false;
}

CG2Node* CNetwork::FindNode(quint32 nAddress)
{
	foreach(CG2Node* pNode, m_lNodes)
	{
		if( pNode->m_oAddress.ip == nAddress )
		{
			return pNode;
		}
	}

	return 0;
}

void CNetwork::AdaptiveHubRun()
{
	if( m_nLeavesConnected == 0 )
		return;

	quint32 nBusyLeaves = 0;

	foreach( CG2Node* pNode, m_lNodes)
	{
		if( pNode->m_nState == nsConnected && pNode->m_nType == G2_LEAF && pNode->m_tRTT >= quazaaSettings.Gnutella2.AdaptiveMaxPing )
		{
			nBusyLeaves++;
		}
	}

	if( nBusyLeaves * 100 / m_nLeavesConnected > quazaaSettings.Gnutella2.AdaptiveBusyPercentage )
	{
		m_nBusyPeriods++;
	}
	m_nTotalPeriods++;

	if( m_nTotalPeriods == quazaaSettings.Gnutella2.AdaptiveTimeWindow )
	{
		if( m_nBusyPeriods * 100 / m_nTotalPeriods > quazaaSettings.Gnutella2.AdaptiveBusyPercentage )
		{
			quazaaSettings.Gnutella2.NumLeafs = qMax<quint32>(m_nLeavesConnected / 2, quazaaSettings.Gnutella2.AdaptiveMinimumLeaves);
		}

		m_nTotalPeriods = m_nBusyPeriods = 0;
	}
}

void CNetwork::ConnectTo(IPv4_ENDPOINT &addr)
{
	CG2Node* pNew = new CG2Node();
	connect(pNew, SIGNAL(NodeStateChanged()), this, SLOT(OnNodeStateChange()));
	emit NodeAdded(pNew);
	m_pRateController->AddSocket(pNew);
	m_lNodes.append(pNew);
	pNew->ConnectTo(addr);;
	pNew->moveToThread(&NetworkThread);
}

// WARNING: pNode must be a valid pointer
void CNetwork::DisconnectFrom(CG2Node *pNode)
{
	pNode->Close();
}
void CNetwork::DisconnectFrom(int index)
{
	if( m_lNodes.size() > index && index >= 0 )
		DisconnectFrom(m_lNodes[index]);
}
void CNetwork::DisconnectFrom(IPv4_ENDPOINT &ip)
{
	for( int i = 0; i < m_lNodes.size(); i++ )
	{
		if( m_lNodes.at(i)->m_oAddress == ip )
		{
			DisconnectFrom(m_lNodes[i]);
			break;
		}
	}
}
void CNetwork::OnSharesReady()
{
	m_bSharesReady = true;
}

void CNetwork::HubBalancing()
{
	systemLog.postLog(LogSeverity::Notice, "*** HUB BALANCING REPORT ***");

	// get the local hub cluster load
	quint32 nLeaves = 0, nMaxLeaves = 0, nClusterLoad = 0, nLocalLoad = 0;

	const quint32 MINUTES_TRYING_BEFORE_SWITCH = 10; // emergency hub switch
	if( !isHub() )
	{
		if( m_nHubsConnected == 0 ) // if we're not connected to any hub
		{
			m_nMinutesTrying++;
			if( m_nMinutesTrying > MINUTES_TRYING_BEFORE_SWITCH ) // if no hub connects in this time
			{
				// emergency switch to hub mode, normal downgrades will filter out bad upgrades
				systemLog.postLog(LogSeverity::Notice, "No HUB connections for %u minutes, switching to HUB mode.", MINUTES_TRYING_BEFORE_SWITCH);
				SwitchClientMode(G2_HUB);
			}
			return;
		}
		else
		{
			m_nMinutesTrying = 0;
		}
	}

	// check how many leaves are in local hub cluster
	foreach( CG2Node* pNode, m_lNodes )
	{
		if( pNode->m_nState == nsConnected && pNode->m_nType == G2_HUB )
		{
			nLeaves += pNode->m_nLeafCount;
			nMaxLeaves += pNode->m_nLeafMax;
		}
	}

	if( isHub() )
	{
		// add our numbers to cluster load
		nLeaves += m_nLeavesConnected;
		nMaxLeaves += quazaaSettings.Gnutella2.NumLeafs;
		// and calculate local hub load percentage
		nLocalLoad = m_nLeavesConnected * 100 / quazaaSettings.Gnutella2.NumLeafs;
		systemLog.postLog(LogSeverity::Notice, "Local Hub load: %u%%, leaves connected: %u, capacity: %u", nLocalLoad, m_nLeavesConnected, quazaaSettings.Gnutella2.NumLeafs);
	}

	// calculate local cluster load percentage
	nClusterLoad = nLeaves * 100 / nMaxLeaves;

	systemLog.postLog(LogSeverity::Notice, "Local Hub Cluster load: %u%%, leaves connected: %u, capacity: %u", nClusterLoad, nLeaves, nMaxLeaves);

	if( nClusterLoad < 50 )
	{
		// if local cluster load is below 50%, increment counter
		m_nMinutesBelow50++;
		systemLog.postLog(LogSeverity::Notice, "Cluster loaded below 50%% for %u minutes.", m_nMinutesBelow50);
	}
	else if( nClusterLoad > 90 )
	{
		// if local cluster load is above 90%, increment counter
		m_nMinutesAbove90++;
		systemLog.postLog(LogSeverity::Notice, "Cluster loaded above 90%% for %u minutes.", m_nMinutesAbove90);
	}
	else
	{
		// reset counters
		m_nMinutesAbove90 = 0;
		m_nMinutesBelow50 = 0;
	}

	if( quazaaSettings.Gnutella2.ClientMode != 0 ) // if client mode is forced in settings
	{
		systemLog.postLog(LogSeverity::Notice, "Not checking for mode change possibility: current client mode forced.");
		return;
	}

	quint32 tNow = time(0);

	const quint32 MODE_CHANGE_WAIT = 1800; // grace period since last mode change
	const quint32 UPGRADE_TIMEOUT = 30; // if cluster loaded for this time - upgrade
	const quint32 DOWNGRADE_TIMEOUT = 60; // if cluster not loaded for this time - downgrade

	if( tNow - m_tLastModeChange < MODE_CHANGE_WAIT )
	{
		// too early for mode change
		systemLog.postLog(LogSeverity::Notice, "Not checking for mode change possibility: too early from last mode change.");
		return;
	}

	if( isHub() && m_nMinutesBelow50 > DOWNGRADE_TIMEOUT )
	{
		// if we're running in hub mode and timeout passed

		if( m_nHubsConnected > 0 ) // if we have connections to other hubs
		{
			if( nLocalLoad > 50 ) // and our load is below 50%
			{
				systemLog.postLog(LogSeverity::Notice, "Cluster load too low for too long, staying in HUB mode, we are above 50%% of our capacity.");
			}
			else
			{
				// switch to leaf mode
				systemLog.postLog(LogSeverity::Notice, "Cluster load too low for too long, switching to LEAF mode.");
				SwitchClientMode(G2_LEAF);
			}
		}
		else
		{
			// no connections to other hubs - stay in HUB mode
			systemLog.postLog(LogSeverity::Notice, "Cluster load too low for too long, staying in HUB mode due to lack of HUB connections.");
		}
	}
	else if( !isHub() && m_nMinutesAbove90 > UPGRADE_TIMEOUT )
	{
		// if we're running in leaf mode and timeout passed

		// TODO: Analyze computers performance and upgrade only if it meets minimum requirements

		if( !IsFirewalled() )
		{
			// switch to HUB mode
			systemLog.postLog(LogSeverity::Notice, "Cluster load too high for too long, switching to HUB mode.");
			SwitchClientMode(G2_HUB);
		}
	}
	else
	{
		systemLog.postLog(LogSeverity::Notice, "No need for mode change.");
	}

}

bool CNetwork::SwitchClientMode(G2NodeType nRequestedMode)
{
	if( !m_bActive )
		return false;

	if( m_nNodeState == nRequestedMode )
		return false;

	m_nMinutesBelow50 = 0;
	m_nMinutesAbove90 = 0;
	m_tLastModeChange = time(0);

	DisconnectAllNodes();
	m_nNodeState = nRequestedMode;

	systemLog.postLog(LogSeverity::Notice, "Switched to %s mode.", (isHub() ? "HUB" : "LEAF"));

	return true;
}
