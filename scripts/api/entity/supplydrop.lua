--- A SupplyDrop is a collectible item picked up on collision with a friendly SpaceShip.
--- On pickup, the SupplyDrop restocks one type of the colliding SpaceShip's weapons.
--- If the ship is a PlayerSpaceship, it can also recharge its energy.
--- A SupplyDrop can also trigger a scripting function upon pickup.
--- For a more generic object with similar collision properties, see Artifact.
--- Example: SupplyDrop():setEnergy(500):setWeaponStorage("Homing",6)
function SupplyDrop()
    local e = createEntity()
    e.transform = {}
    for k, v in pairs(__model_data["ammo_box"]) do
        if string.sub(1, 2) ~= "__" then
            e[k] = table.deepcopy(v)
        end
    end
    e.physics = {type="Sensor"}

    e.radar_trace = {
        color={100, 200, 255},
        icon="radar/blip.png",
        radius=120.0,
        rotate=false,
        color_by_faction=true,
    }
    e.pickup = {}

    return e
end