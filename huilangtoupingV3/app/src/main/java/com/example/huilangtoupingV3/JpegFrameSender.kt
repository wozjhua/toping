package com.example.huilangtoupingV3

import android.net.LocalServerSocket
import android.net.LocalSocket
import android.util.Log
import java.io.BufferedReader
import java.io.InputStreamReader

class JpegFrameSender(
    private val socketName: String,
    private val onStatusChanged: ((String) -> Unit)? = null,
    private val onPerfText: ((String) -> Unit)? = null,
    private val onControlCommand: ((String) -> Unit)? = null,
) {
    private val lock = Object()

    @Volatile
    private var closed = false

    @Volatile
    var isConnected: Boolean = false
        private set

    private var worker: Thread? = null

    @Volatile
    private var lastStatus: String? = null

    private fun updateStatus(text: String) {
        if (lastStatus == text) return
        lastStatus = text
        runCatching { onStatusChanged?.invoke(text) }
    }

    private fun startControlReader(socket: LocalSocket): Thread {
        return Thread({
            try {
                val reader = BufferedReader(InputStreamReader(socket.inputStream, Charsets.UTF_8))
                while (!closed && isConnected) {
                    val line = reader.readLine() ?: break
                    val text = line.trim()
                    if (text.isNotEmpty()) {
                        runCatching { onControlCommand?.invoke(text) }
                    }
                }
            } catch (t: Throwable) {
                if (!closed) {
                    Log.d("JpegFrameSender", "control reader stopped: ${t.javaClass.simpleName}")
                }
            }
        }, "JpegFrameControlReader").apply {
            isDaemon = true
            start()
        }
    }

    fun start() {
        synchronized(lock) {
            if (worker != null) return
            closed = false
            worker = Thread({ runLoop() }, "JpegFrameSender").apply {
                isDaemon = true
                start()
            }
        }
    }

    fun close() {
        val thread = synchronized(lock) {
            if (closed) return
            closed = true
            NativeCenterCropJpegEncoder.clearOutputFileDescriptor()
            isConnected = false
            worker.also { worker = null }
        }
        runCatching { thread?.join(700) }
    }

    private fun runLoop() {
        var server: LocalServerSocket? = null
        var client: LocalSocket? = null

        try {
            try {
                server = LocalServerSocket(socketName)
            } catch (t: Throwable) {
                updateStatus("画面传输：创建本地通道失败 ${t.javaClass.simpleName}")
                Log.e("JpegFrameSender", "create LocalServerSocket failed name=$socketName", t)
                return
            }

            updateStatus("画面传输：等待 PC 连接 ($socketName)")

            while (!closed) {
                try {
                    runCatching { client?.close() }
                    client = null
                    NativeCenterCropJpegEncoder.clearOutputFileDescriptor()
                    isConnected = false

                    val accepted = server.accept()
                    if (closed) {
                        runCatching { accepted.close() }
                        return
                    }
                    client = accepted

                    accepted.sendBufferSize = 1024 * 1024
                    accepted.receiveBufferSize = 64 * 1024

                    val fdReady = NativeCenterCropJpegEncoder.setOutputFileDescriptor(accepted.fileDescriptor)
                    if (!fdReady) {
                        updateStatus("画面传输：  通道设置失败")
                        runCatching { accepted.close() }
                        client = null
                        Thread.sleep(200)
                        continue
                    }

                    isConnected = true
                    startControlReader(accepted)
                    updateStatus("画面传输：PC 已连接  ")

                    while (!closed && isConnected) {
                        Thread.sleep(300)
                        if (!NativeCenterCropJpegEncoder.hasOutputFileDescriptor()) {
                            throw IllegalStateException(" ")
                        }
                    }
                } catch (t: Throwable) {
                    if (!closed) {
                        val detail = buildString {
                            append(t.javaClass.simpleName)
                            if (!t.message.isNullOrBlank()) {
                                append(": ")
                                append(t.message)
                            }
                        }
                        updateStatus("画面传输：连接已断开 $detail")
                        Log.e("JpegFrameSender", "  socket loop failed socketName=$socketName", t)
                        Thread.sleep(200)
                    }
                } finally {
                    NativeCenterCropJpegEncoder.clearOutputFileDescriptor()
                    isConnected = false
                    runCatching { client?.close() }
                    client = null
                    if (!closed) {
                        updateStatus("画面传输：等待 PC 连接 ($socketName)")
                    }
                }
            }
        } finally {
            NativeCenterCropJpegEncoder.clearOutputFileDescriptor()
            isConnected = false
            runCatching { client?.close() }
            runCatching { server?.close() }
            updateStatus("画面传输：未启动")
        }
    }
}
