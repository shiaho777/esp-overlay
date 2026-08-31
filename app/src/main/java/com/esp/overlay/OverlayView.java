package com.esp.overlay;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.PorterDuff;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;

import com.esp.core.Entity;
import com.esp.core.EspNative;

import java.util.Arrays;

/**
 * SurfaceView-based overlay that draws ESP boxes on top of the game.
 * Uses TYPE_APPLICATION_OVERLAY window — no root required.
 */
public class OverlayView extends SurfaceView implements SurfaceHolder.Callback, Runnable {

    private final WindowManager windowManager;
    private final WindowManager.LayoutParams params;
    private SurfaceHolder holder;
    private Thread drawThread;
    private volatile boolean drawing = false;

    // Paints
    private final Paint enemyBoxPaint;
    private final Paint allyBoxPaint;
    private final Paint textPaint;
    private final Paint linePaint;
    private final Paint hpBgPaint;
    private final Paint hpFgPaint;

    private int screenWidth;
    private int screenHeight;

    // Config
    private boolean showEnemies = true;
    private boolean showAllies = false;
    private boolean showMinions = false;
    private boolean showJungle = false;
    private boolean showHp = true;
    private boolean showNames = false;
    private boolean showDistance = true;
    private boolean showBoxes = true;
    private boolean showLines = false;

    public OverlayView(Context context) {
        super(context);

        windowManager = (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);

        holder = getHolder();
        holder.addCallback(this);
        setZOrderOnTop(true);
        holder.setFormat(PixelFormat.TRANSLUCENT);

        // Window parameters for overlay
        params = new WindowManager.LayoutParams(
            WindowManager.LayoutParams.MATCH_PARENT,
            WindowManager.LayoutParams.MATCH_PARENT,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE |
                WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE |
                WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS |
                WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED,
            PixelFormat.TRANSLUCENT
        );

        // Initialize paints
        enemyBoxPaint = new Paint();
        enemyBoxPaint.setColor(Color.RED);
        enemyBoxPaint.setStyle(Paint.Style.STROKE);
        enemyBoxPaint.setStrokeWidth(3f);
        enemyBoxPaint.setAntiAlias(true);

        allyBoxPaint = new Paint();
        allyBoxPaint.setColor(Color.GREEN);
        allyBoxPaint.setStyle(Paint.Style.STROKE);
        allyBoxPaint.setStrokeWidth(3f);
        allyBoxPaint.setAntiAlias(true);

        textPaint = new Paint();
        textPaint.setColor(Color.WHITE);
        textPaint.setTextSize(28f);
        textPaint.setAntiAlias(true);
        textPaint.setShadowLayer(2f, 1f, 1f, Color.BLACK);

        linePaint = new Paint();
        linePaint.setColor(Color.argb(120, 255, 0, 0));
        linePaint.setStrokeWidth(2f);
        linePaint.setAntiAlias(true);

        hpBgPaint = new Paint();
        hpBgPaint.setColor(Color.argb(180, 0, 0, 0));
        hpBgPaint.setStyle(Paint.Style.FILL);

        hpFgPaint = new Paint();
        hpFgPaint.setColor(Color.GREEN);
        hpFgPaint.setStyle(Paint.Style.FILL);
    }

    public void attachToWindow() {
        try {
            windowManager.addView(this, params);
        } catch (Exception e) {
            // Permission not granted or window already added
        }
    }

    public void detachFromWindow() {
        try {
            windowManager.removeView(this);
        } catch (Exception e) {
            // Already removed
        }
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        screenWidth = getWidth();
        screenHeight = getHeight();
        if (EspNative.isRunning()) {
            EspNative.setScreenSize(screenWidth, screenHeight);
        }
        drawing = true;
        drawThread = new Thread(this);
        drawThread.start();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        screenWidth = width;
        screenHeight = height;
        if (EspNative.isRunning()) {
            EspNative.setScreenSize(width, height);
        }
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        drawing = false;
        if (drawThread != null) {
            try { drawThread.join(100); } catch (InterruptedException e) {}
            drawThread = null;
        }
    }

    @Override
    public void run() {
        while (drawing) {
            Canvas canvas = null;
            try {
                canvas = holder.lockCanvas();
                if (canvas != null) {
                    drawFrame(canvas);
                }
            } catch (Exception e) {
                // Surface not ready
            } finally {
                if (canvas != null) {
                    try { holder.unlockCanvasAndPost(canvas); } catch (Exception e) {}
                }
            }
            try { Thread.sleep(16); } catch (InterruptedException e) { break; }
        }
    }

    private void drawFrame(Canvas canvas) {
        // Clear
        canvas.drawColor(Color.TRANSPARENT, PorterDuff.Mode.CLEAR);

        if (!EspNative.isRunning()) return;

        Entity[] entities = EspNative.getEntities();
        if (entities == null) return;

        for (Entity e : entities) {
            if (!e.visible) continue;

            float x = e.screenX;
            float y = e.screenY;

            // Skip off-screen entities
            if (x < -100 || x > screenWidth + 100 || y < -100 || y > screenHeight + 100) {
                continue;
            }

            // Filter by config
            if (e.isEnemy() && e.isHero() && !showEnemies) continue;
            if (!e.isEnemy() && e.isHero() && !showAllies) continue;
            if (e.isJungle() && !showJungle) continue;

            Paint boxPaint = e.isEnemy() ? enemyBoxPaint : allyBoxPaint;

            // Draw box
            if (showBoxes) {
                float boxW = 80f;
                float boxH = 120f;
                canvas.drawRect(x - boxW/2, y - boxH/2, x + boxW/2, y + boxH/2, boxPaint);
            }

            // Draw dot at position
            canvas.drawCircle(x, y, 5f, boxPaint);

            // Draw line from bottom center to entity
            if (showLines) {
                canvas.drawLine(screenWidth / 2f, screenHeight, x, y, linePaint);
            }

            // Draw label
            float labelY = y - 70f;
            String label = "";
            if (e.isHero()) {
                label = e.isEnemy() ? "ENEMY" : "ALLY";
            } else if (e.isJungle()) {
                label = "JUNGLE";
            }

            if (!label.isEmpty()) {
                canvas.drawText(label, x - 30f, labelY, textPaint);
            }
        }
    }

    public void updateConfig(boolean enemies, boolean allies, boolean minions,
                            boolean jungle, boolean hp, boolean names,
                            boolean distance, boolean boxes, boolean lines) {
        this.showEnemies = enemies;
        this.showAllies = allies;
        this.showMinions = minions;
        this.showJungle = jungle;
        this.showHp = hp;
        this.showNames = names;
        this.showDistance = distance;
        this.showBoxes = boxes;
        this.showLines = lines;
        EspNative.setConfig(enemies, allies, minions, jungle, hp, names, distance, boxes, lines);
    }
}
