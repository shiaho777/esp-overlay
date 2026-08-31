package com.esp.core;

/**
 * Entity data passed from native to Java for rendering.
 */
public class Entity {
    public final float screenX;
    public final float screenY;
    public final float worldX;
    public final float worldZ;
    public final int actorType;
    public final int camp;
    public final boolean visible;

    public Entity(float screenX, float screenY, float worldX, float worldZ,
                 int actorType, int camp, boolean visible) {
        this.screenX = screenX;
        this.screenY = screenY;
        this.worldX = worldX;
        this.worldZ = worldZ;
        this.actorType = actorType;
        this.camp = camp;
        this.visible = visible;
    }

    /** ACTOR_TYPE_HERO = 1 */
    public boolean isHero() { return actorType == 1; }
    /** ACTOR_TYPE_MONSTER (jungle) = 4 */
    public boolean isJungle() { return actorType == 4; }
    /** Enemy camp (camp == 1 in this game) */
    public boolean isEnemy() { return camp == 1; }
}
