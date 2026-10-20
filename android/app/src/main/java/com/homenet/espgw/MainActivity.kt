package com.homenet.espgw

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import com.homenet.espgw.ui.EspGwApp
import com.homenet.espgw.ui.theme.EspGwTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            EspGwTheme {
                EspGwApp()
            }
        }
    }
}
