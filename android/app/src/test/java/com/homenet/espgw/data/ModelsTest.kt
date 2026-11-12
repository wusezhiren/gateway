package com.homenet.espgw.data

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class ModelsTest {
    @Test fun parsesCompleteGatewayAndPendingNode() {
        val info = GwInfo.parse(JSONObject("""{
          "project":"esp_s3_gateway","version":"0.7.0","wifi":{"connected":true,"ip":"192.168.31.121"},
          "nodes":{"total":2,"online":1},"ota":{"state":"idle"},
          "node_ota":{"state":"sending","received":50,"total":100},"cloud":{"connected":true}
        }"""))
        assertEquals("0.7.0", info.version)
        assertEquals(1, info.nodesOnline)
        assertTrue(info.nodeOta.active)
        assertTrue(info.cloudConnected)
        val nodes = NodeList.parse(JSONObject("""{"nodes":[{
          "mac":"aa","name":"卧室","ch":[{"type":"relay","value":1,"desired":0}]
        }]}"""))
        assertEquals(1, nodes.nodes[0].ch[0].value)
        assertEquals(0, nodes.nodes[0].ch[0].desired)
    }

    @Test fun missingFieldsAndOfflineDefaults() {
        val info = GwInfo.parse(JSONObject("{}"))
        assertFalse(info.wifiConnected)
        assertFalse(info.cloudConnected)
        assertEquals("idle", info.nodeOta.state)
        assertFalse(info.nodeOta.active)
        val nodes = NodeList.parse(JSONObject("""{"nodes":[{"mac":"aa","ch":[{"value":0}]}]}"""))
        assertEquals("aa", nodes.nodes[0].title)
        assertNull(nodes.nodes[0].ch[0].desired)
        assertEquals(-1, nodes.nodes[0].lastSeenS)
        assertTrue(NodeList.parse(JSONObject("{} ")).nodes.isEmpty())
    }
}
