package com.homenet.espgw

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import com.homenet.espgw.ui.EspGwApp
import com.homenet.espgw.ui.theme.EspGwTheme
import com.homenet.espgw.widget.LampWidget

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

    /** 在 App 里改过开关后，回到桌面时小组件显示最新状态 */
    override fun onStop() {
        super.onStop()
        LampWidget.refresh(this)
    }
}
