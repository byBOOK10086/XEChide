package dev.xechide.manager

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import dev.xechide.manager.ui.ManagerApp
import dev.xechide.manager.ui.theme.XeChideTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            XeChideTheme {
                ManagerApp()
            }
        }
    }
}
