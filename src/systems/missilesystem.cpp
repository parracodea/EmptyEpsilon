#include "systems/missilesystem.h"

#include "components/collision.h"
#include "components/missiletubes.h"
#include "components/missile.h"
#include "components/hull.h"
#include "components/radar.h"
#include "components/docking.h"
#include "components/warpdrive.h"
#include "ecs/query.h"
#include "multiplayer_server.h"
#include "spaceObjects/explosionEffect.h"
#include "spaceObjects/electricExplosionEffect.h"
#include "particleEffect.h"
#include "random.h"


MissileSystem::MissileSystem()
{
    sp::CollisionSystem::addHandler(this);
}

void MissileSystem::update(float delta)
{
    for(auto [entity, tubes] : sp::ecs::Query<MissileTubes>())
    {
        for(auto& tube : tubes.mounts)
        {
            if (tube.delay > 0.0f)
            {
                tube.delay -= delta * tubes.getSystemEffectiveness();
            }else{
                switch(tube.state)
                {
                case MissileTubes::MountPoint::State::Loading:
                    tube.state = MissileTubes::MountPoint::State::Loaded;
                    break;
                case MissileTubes::MountPoint::State::Unloading:
                    tube.state = MissileTubes::MountPoint::State::Empty;
                    if (tubes.storage[tube.type_loaded] < tubes.storage_max[tube.type_loaded])
                        tubes.storage[tube.type_loaded]++;
                    tube.type_loaded = MW_None;
                    break;
                case MissileTubes::MountPoint::State::Firing:
                    if (game_server)
                    {
                        spawnProjectile(entity, tube, 0, {});

                        tube.fire_count -= 1;
                        if (tube.fire_count > 0)
                        {
                            tube.delay = 1.5;
                        }
                        else
                        {
                            tube.state = MissileTubes::MountPoint::State::Empty;
                            tube.type_loaded = MW_None;
                        }
                    }
                    break;
                default:
                    break;
                }
            }
        }
    }

    for(auto [entity, flight, transform, physics] : sp::ecs::Query<MissileFlight, sp::Transform, sp::Physics>()) {
        if (flight.timeout > 0.0f) {
            flight.timeout -= delta;
            if (flight.timeout <= 0.0f && game_server) {
                entity.removeComponent<MissileFlight>();
            }
        }
        physics.setVelocity(vec2FromAngle(transform.getRotation()) * flight.speed);
    }

    for(auto [entity, homing, transform, physics] : sp::ecs::Query<MissileHoming, sp::Transform, sp::Physics>()) {
        if (auto tt = homing.target.getComponent<sp::Transform>()) {
            float r = homing.range + 10.0f;
            if (glm::length2(tt->getPosition() - transform.getPosition()) < r*r)
                homing.target_angle = vec2ToAngle(tt->getPosition() - transform.getPosition());
        }
        float angle_diff = angleDifference(transform.getRotation(), homing.target_angle);

        if (angle_diff > 1.0f)
            physics.setAngularVelocity(homing.turn_rate);
        else if (angle_diff < -1.0f)
            physics.setAngularVelocity(homing.turn_rate * -1.0f);
        else
            physics.setAngularVelocity(angle_diff * homing.turn_rate);
    }

    // TODO: Not really part of missile
    for(auto [entity, emitter, transform] : sp::ecs::Query<ConstantParticleEmitter, sp::Transform>()) {
        emitter.delay -= delta;
        if (emitter.delay <= 0.0f) {
            emitter.delay = emitter.interval;
            auto pos = glm::vec3(transform.getPosition().x, transform.getPosition().y, 0);
            ParticleEngine::spawn(pos, pos + glm::vec3(random(-emitter.travel_random_range, emitter.travel_random_range), random(-emitter.travel_random_range, emitter.travel_random_range), random(-emitter.travel_random_range, emitter.travel_random_range)), emitter.start_color, emitter.end_color, emitter.start_size, emitter.end_size, emitter.life_time);
        }
    }

    if (game_server) {
        for(auto [entity, deot, transform] : sp::ecs::Query<DelayedExplodeOnTouch, sp::Transform>()) {
            if (!deot.triggered) continue;
            deot.delay -= delta;
            if (deot.delay >= 0.0f) continue;

            explode(entity, {}, deot);
        }
    }

    if (game_server) {
        // TODO: Not really part of missile
        for(auto [entity, lifetime] : sp::ecs::Query<LifeTime>()) {
            lifetime.lifetime -= delta;
            if (lifetime.lifetime <= 0.0f) {
                //TODO: Nukes/EMPs should explode.
                entity.destroy();
            }
        }
    }
}

void MissileSystem::collision(sp::ecs::Entity a, sp::ecs::Entity b, float force)
{
    if (!game_server) return;
    auto deot = a.getComponent<DelayedExplodeOnTouch>();
    if (deot) {
        auto hull = b.getComponent<Hull>();
        if (!hull) return;
        deot->triggered = true;
    }
    auto eot = a.getComponent<ExplodeOnTouch>();
    if (eot) return;
    if (eot->owner == b) return;
    auto hull = b.getComponent<Hull>();
    if (!hull) return;
    if (!(hull->damaged_by_flags & (1 << int(eot->damage_type)))) return;

    explode(a, b, *eot);
}

void MissileSystem::explode(sp::ecs::Entity source, sp::ecs::Entity target, ExplodeOnTouch& eot)
{
    auto transform = source.getComponent<sp::Transform>();
    if (!transform) return;
    DamageInfo info(eot.owner, eot.damage_type, transform->getPosition());
    if (eot.blast_range > 100.0f || !target) {
        DamageSystem::damageArea(transform->getPosition(), eot.blast_range, eot.damage_at_edge, eot.damage_at_center, info, eot.blast_range / 2);
    } else {
        DamageSystem::applyDamage(target, eot.damage_at_center, info);
    }

    if (eot.damage_type == DamageType::EMP) {
        P<ElectricExplosionEffect> e = new ElectricExplosionEffect();
        e->setSize(eot.blast_range);
        e->setPosition(transform->getPosition());
        e->setOnRadar(true);
    } else {
        P<ExplosionEffect> e = new ExplosionEffect();
        e->setSize(eot.blast_range);
        e->setPosition(transform->getPosition());
        e->setOnRadar(true);
        if (!eot.explosion_sfx.empty())
            e->setExplosionSound(eot.explosion_sfx);
    }
    source.destroy();
}

void MissileSystem::startLoad(sp::ecs::Entity source, MissileTubes::MountPoint& tube, EMissileWeapons type)
{
    if (!tube.canLoad(type))
        return;
    if (tube.state != MissileTubes::MountPoint::State::Empty)
        return;
    auto tubes = source.getComponent<MissileTubes>();
    if (!tubes) return;
    if (tubes->storage[type] <= 0)
        return;

    tube.state = MissileTubes::MountPoint::State::Loading;
    tube.delay = tube.load_time;
    tube.type_loaded = type;
    tubes->storage[type]--;
}

void MissileSystem::startUnload(sp::ecs::Entity source, MissileTubes::MountPoint& tube)
{
    if (tube.state == MissileTubes::MountPoint::State::Loaded)
    {
        tube.state = MissileTubes::MountPoint::State::Unloading;
        tube.delay = tube.load_time;
    }
}

void MissileSystem::fire(sp::ecs::Entity source, MissileTubes::MountPoint& tube, float target_angle, sp::ecs::Entity target)
{
    Faction::didAnOffensiveAction(source);

    auto docking_port = source.getComponent<DockingPort>();
    if (docking_port && docking_port->state != DockingPort::State::NotDocking) return;

    auto warp = source.getComponent<WarpDrive>();
    if (warp && warp->current > 0.0f) return;
    if (tube.state != MissileTubes::MountPoint::State::Loaded) return;

    if (tube.type_loaded == MW_HVLI)
    {
        tube.fire_count = 5;
        tube.state = MissileTubes::MountPoint::State::Firing;
        tube.delay = 0.0;
    }else{
        spawnProjectile(source, tube, target_angle, target);
        tube.state = MissileTubes::MountPoint::State::Empty;
        tube.type_loaded = MW_None;
    }
}

void MissileSystem::spawnProjectile(sp::ecs::Entity source, MissileTubes::MountPoint& tube, float target_angle, sp::ecs::Entity target)
{
    auto source_transform = source.getComponent<sp::Transform>();
    if (!source_transform) return;
    auto fireLocation = source_transform->getPosition() + rotateVec2(glm::vec2(tube.position), source_transform->getRotation());
    auto category_modifier = MissileWeaponData::convertSizeToCategoryModifier(tube.size);

    sp::ecs::Entity missile;
    switch(tube.type_loaded)
    {
    case MW_Homing:
        {
            missile = sp::ecs::Entity::create();
            auto& mc = missile.addComponent<ExplodeOnTouch>();
            mc.owner = source;
            mc.damage_at_center = 35 * category_modifier;
            mc.damage_at_edge = 5 * category_modifier;
            mc.blast_range = 30 * category_modifier;
            missile.addComponent<RawRadarSignatureInfo>(0.0f, 0.1f, 0.2f);
        }
        break;
    case MW_Nuke:
        {
            missile = sp::ecs::Entity::create();
            auto& mc = missile.addComponent<ExplodeOnTouch>();
            mc.owner = source;
            mc.damage_at_center = 160.0f * category_modifier;
            mc.damage_at_edge = 30.0f * category_modifier;
            mc.blast_range = 1000.0f * category_modifier;
            mc.explosion_sfx = "sfx/nuke_explosion.wav";
            missile.addComponent<RawRadarSignatureInfo>(0.0f, 0.7f, 0.1f);
            //TODO: Add avoid area after X time
        }
        break;
    case MW_Mine:
        {
            missile = sp::ecs::Entity::create();
            auto& mc = missile.addComponent<DelayedExplodeOnTouch>();
            mc.delay = 1.0f;
            mc.owner = source;
            mc.damage_at_center = 160.0f * category_modifier;
            mc.damage_at_edge = 30.0f * category_modifier;
            mc.blast_range = 1000.0f * category_modifier;
            missile.addComponent<RawRadarSignatureInfo>(0.0f, 0.05f, 0.0f);
        }
        break;
    case MW_HVLI:
        {
            missile = sp::ecs::Entity::create();
            auto& mc = missile.addComponent<ExplodeOnTouch>();
            mc.owner = source;
            mc.damage_at_center = 10.0f * category_modifier;
            mc.damage_at_edge = 10.0f * category_modifier;
            mc.blast_range = 20.0f * category_modifier;
            missile.addComponent<RawRadarSignatureInfo>(0.1f, 0.0f, 0.0f);
        }
        break;
    case MW_EMP:
        {
            missile = sp::ecs::Entity::create();
            auto& mc = missile.addComponent<ExplodeOnTouch>();
            mc.owner = source;
            mc.damage_at_center = 160.0f * category_modifier;
            mc.damage_at_edge = 30.0f * category_modifier;
            mc.blast_range = 1000.0f * category_modifier;
            mc.damage_type = DamageType::EMP;
            missile.addComponent<RawRadarSignatureInfo>(0.0f, 1.0f, 0.0f);
        }
        break;
    default:
        break;
    }

    if (missile) {
        auto& physics = missile.addComponent<sp::Physics>();
        if (tube.type_loaded == MW_Mine)
            physics.setCircle(sp::Physics::Type::Sensor, 1000.0f * 0.6f);
        else
            physics.setRectangle(sp::Physics::Type::Sensor, {10, 30});

        auto& mwd = MissileWeaponData::getDataFor(tube.type_loaded);
        auto& mf = missile.addComponent<MissileFlight>();
        mf.speed = mwd.speed / category_modifier;
        if (tube.type_loaded == MW_Mine)
            mf.timeout = mwd.lifetime;
        if (mwd.homing_range > 0.0f) {
            auto& mh = missile.addComponent<MissileHoming>();
            mh.range = mwd.homing_range;
            mh.target = target;
            mh.target_angle = target_angle;
            mh.turn_rate = mwd.turnrate / category_modifier;
        }

        if (auto f = source.getComponent<Faction>())
            missile.addComponent<Faction>().entity = f->entity;
        auto& t = missile.addComponent<sp::Transform>();
        t.setPosition(fireLocation);
        t.setRotation(source_transform->getRotation() + tube.direction);
        auto cpe = missile.addComponent<ConstantParticleEmitter>();
        if (tube.type_loaded == MW_Mine) {
            cpe.travel_random_range = 100.0f;
            cpe.start_color = {1, 1, 1};
            cpe.end_color = {0, 0, 1};
            cpe.interval = 0.4;
            cpe.start_size = 30.0f;
            cpe.end_size = 0.0f;
            cpe.life_time = 10.0f;
        }

        missile.addComponent<LifeTime>().lifetime = mwd.lifetime / category_modifier;

        if (tube.type_loaded != MW_Mine) {
            auto& hull = missile.addComponent<Hull>();
            hull.max = hull.current = 1;
            hull.damaged_by_flags = (1 << int(DamageType::EMP)) | (1 << int(DamageType::Energy));
        }

        auto& trace = missile.addComponent<RadarTrace>();
        if (tube.type_loaded == MW_Mine)
            trace.icon = "radar/blip.png";
        else
            trace.icon = "radar/arrow.png";
        trace.radius = 32.0f;
        trace.max_size = trace.min_size = 32 * (0.25f + 0.25f * category_modifier);
        trace.flags = RadarTrace::Rotate;
        trace.color = mwd.color;
    }
}

float MissileSystem::calculateFiringSolution(sp::ecs::Entity source, const MissileTubes::MountPoint& tube, sp::ecs::Entity target)
{
    if (!target)
        return std::numeric_limits<float>::infinity();
    const MissileWeaponData& data = MissileWeaponData::getDataFor(tube.type_loaded);
    if (data.turnrate == 0.0f)  //If the missile cannot turn, we cannot find a firing solution.
        return std::numeric_limits<float>::infinity();
    auto source_transform = source.getComponent<sp::Transform>();
    if (!source_transform)
        return std::numeric_limits<float>::infinity();
    auto target_transform = target.getComponent<sp::Transform>();
    if (!target_transform)
        return std::numeric_limits<float>::infinity();
    auto target_physics = target.getComponent<sp::Physics>();

    auto target_position = target_transform->getPosition();
    auto target_velocity = target_physics ? target_physics->getVelocity() : glm::vec2{0, 0};
    float target_velocity_length = glm::length(target_velocity);
    float missile_angle = vec2ToAngle(target_position - source_transform->getPosition());
    float turn_radius = ((360.0f / data.turnrate) * data.speed) / (2.0f * float(M_PI));
    float missile_exit_angle = source_transform->getRotation() + tube.direction;
    float target_radius = target_physics ? target_physics->getSize().x : 100.0f;

    for(int iterations=0; iterations<10; iterations++)
    {
        float angle_diff = angleDifference(missile_angle, missile_exit_angle);

        float left_or_right = 90;
        if (angle_diff > 0)
            left_or_right = -90;

        auto turn_center = source_transform->getPosition() + vec2FromAngle(missile_exit_angle + left_or_right) * turn_radius;
        auto turn_exit = turn_center + vec2FromAngle(missile_angle - left_or_right) * turn_radius;
        if (target_velocity_length < 1.0f)
        {
            //If the target is almost standing still, just target the position directly instead of using the velocity of the target in the calculations.
            float time_missile = glm::length(turn_exit - target_position) / data.speed;
            auto interception = turn_exit + vec2FromAngle(missile_angle) * data.speed * time_missile;
            float r = target_radius * 0.5f;
            if (glm::length2(interception - target_position) < r*r)
                return missile_angle;
            missile_angle = vec2ToAngle(target_position - turn_exit);
        }
        else
        {
            auto missile_velocity = vec2FromAngle(missile_angle) * data.speed;
            //Calculate the position where missile and the target will cross each others path.
            auto intersection = lineLineIntersection(target_position, target_position + target_velocity, turn_exit, turn_exit + missile_velocity);
            //Calculate the time it will take for the target and missile to reach the intersection
            float turn_time = fabs(angle_diff) / data.turnrate;
            float time_target = glm::length((target_position - intersection)) / target_velocity_length;
            float time_missile = glm::length(turn_exit - intersection) / data.speed + turn_time;
            //Calculate the time in which the radius will be on the intersection, to know in which time range we need to hit.
            float time_radius = (target_radius * 0.5f) / target_velocity_length;//TODO: This value could be improved, as it is allowed to be bigger when the angle between the missile and the ship is low
            // When both the missile and the target are at the same position at the same time, we can take a shot!
            if (fabsf(time_target - time_missile) < time_radius)
                return missile_angle;

            //When we cannot hit the target with this setup yet. Calculate a new intersection target, and aim for that.
            float guessed_impact_time = (time_target * target_velocity_length / (target_velocity_length + data.speed)) + (time_missile * data.speed / (target_velocity_length + data.speed));
            auto new_target_position = target_position + target_velocity * guessed_impact_time;
            missile_angle = vec2ToAngle(new_target_position - turn_exit);
        }
    }
    return std::numeric_limits<float>::infinity();
}