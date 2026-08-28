package com.jitong.im.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.jitong.im.data.Prefs
import com.jitong.im.ui.theme.JitongBlue
import com.jitong.im.ui.theme.PageBackground

@Composable
fun LoginScreen(vm: MainViewModel) {
    val tip by vm.loginTip.collectAsStateWithLifecycle()
    var tab by rememberSaveable { mutableIntStateOf(0) }
    var nick by rememberSaveable { mutableStateOf("") }
    var tel by rememberSaveable { mutableStateOf(Prefs.tel.orEmpty()) }
    var pass by rememberSaveable { mutableStateOf(Prefs.pass.orEmpty()) }
    var remember by rememberSaveable { mutableStateOf(Prefs.remember) }

    Box(
        Modifier.fillMaxSize().background(PageBackground).systemBarsPadding(),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            Modifier.fillMaxWidth().padding(horizontal = 28.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            JitongLogo(size = 104.dp)
            Spacer(Modifier.height(10.dp))
            Text("即通", style = MaterialTheme.typography.headlineLarge, fontWeight = FontWeight.Bold)
            Text("让沟通，简单抵达", color = Color(0xFF7D8898), style = MaterialTheme.typography.bodyMedium)
            Spacer(Modifier.height(30.dp))

            Row(
                Modifier.fillMaxWidth().height(44.dp).background(Color(0xFFEAF3FF), RoundedCornerShape(14.dp)),
            ) {
                LoginTab("登录", tab == 0, Modifier.weight(1f)) { tab = 0 }
                LoginTab("注册", tab == 1, Modifier.weight(1f)) { tab = 1 }
            }
            Spacer(Modifier.height(18.dp))

            if (tab == 1) {
                JitongField(nick, { nick = it }, "昵称", maxLength = 20)
                Spacer(Modifier.height(12.dp))
            }
            JitongField(tel, { tel = it.filter(Char::isDigit) }, "手机号", KeyboardType.Phone, maxLength = 11)
            Spacer(Modifier.height(12.dp))
            JitongField(pass, { pass = it }, "密码（6～64 位）", KeyboardType.Password, true, 64)
            Spacer(Modifier.height(12.dp))

            if (tab == 0) {
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(checked = remember, onCheckedChange = { remember = it })
                    Text("记住账号", Modifier.clickable { remember = !remember })
                    Spacer(Modifier.weight(1f))
                    TextButton(onClick = { vm.notify("请联系管理员重置密码") }) { Text("忘记密码？") }
                }
            }
            Button(
                onClick = { if (tab == 0) vm.login(tel, pass, remember) else vm.register(nick, tel, pass) },
                modifier = Modifier.fillMaxWidth().height(52.dp),
                shape = RoundedCornerShape(14.dp),
            ) { Text(if (tab == 0) "登录" else "注册", fontWeight = FontWeight.SemiBold) }

            if (tip.isNotEmpty()) {
                Spacer(Modifier.height(14.dp))
                Text(tip, color = if (tip.contains("失败") || tip.contains("请输入")) MaterialTheme.colorScheme.error else JitongBlue)
            }
        }
    }
}

@Composable
private fun LoginTab(text: String, selected: Boolean, modifier: Modifier, onClick: () -> Unit) {
    Box(
        modifier.fillMaxHeight().padding(3.dp)
            .background(if (selected) Color.White else Color.Transparent, RoundedCornerShape(12.dp))
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) { Text(text, color = if (selected) JitongBlue else Color(0xFF606B7A), fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Normal) }
}

@Composable
private fun JitongField(
    value: String,
    onValueChange: (String) -> Unit,
    label: String,
    keyboardType: KeyboardType = KeyboardType.Text,
    password: Boolean = false,
    maxLength: Int = Int.MAX_VALUE,
) {
    OutlinedTextField(
        value = value,
        onValueChange = { if (it.length <= maxLength) onValueChange(it) },
        placeholder = { Text(label) },
        singleLine = true,
        visualTransformation = if (password) PasswordVisualTransformation() else androidx.compose.ui.text.input.VisualTransformation.None,
        keyboardOptions = KeyboardOptions(keyboardType = keyboardType),
        shape = RoundedCornerShape(14.dp),
        colors = OutlinedTextFieldDefaults.colors(
            focusedContainerColor = Color.White,
            unfocusedContainerColor = Color.White,
            unfocusedBorderColor = Color(0xFFDCE4EF),
        ),
        modifier = Modifier.fillMaxWidth(),
    )
}
