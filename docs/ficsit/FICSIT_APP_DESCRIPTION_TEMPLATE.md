# Auto-Connecting Power Rails

**A power backbone for modular factories.** Rails and junctions carry power like beams carry weight: snap them together, bridge them with blueprints, and plug the rest of your factory in through outlets — or skip the cables and let a wireless outlet reach the machines.

![Overview: a rail run with a junction, an outlet and a few power lines leading off to machines](https://raw.githubusercontent.com/ZeeOcho/AutoConnectingPowerRails/main/docs/images/overview.jpg)

---

## What it adds

| | |
| --- | --- |
| **Power Rail** | A conductive beam, 1 to 40 m, placed like a painted beam. Its two ends are *terminals*: aim a new rail at one and it snaps and connects, straight on. |
| **Power Rail Junction** | A 1 m cube with a terminal on every face. Place it free, snap it onto a rail's end, or drop it onto the middle of a rail to split the rail in two. Up to six rails meet in one node. |
| **Power Rail Outlet** | The way in and out. Mounts on a rail's body or on an open terminal, and takes up to four ordinary power lines, like a Wall Outlet. A power line aimed at a rail builds one for you. |
| **Power Rail Wireless Outlet** Mk.1 / Mk.2 / Mk.3 | Mounts like an outlet and powers every unwired machine within 8 / 16 / 32 m — no cable at all. |
| **Insulated Terminal Cap** | Closes a terminal off. A capped end never connects to anything, by hand or by blueprint. |

Everything is unlocked at once by the **Power Rails** package in the AWESOME Shop (4 FICSIT Coupons, visible after Basic Steel Production). Rails cost 1 Steel Beam + 1 Cable per 10 m, a junction the same, an outlet is priced like a Wall Outlet, a cap is 1 Concrete. The wireless outlets cost 4 / 8 / 16 Quickwire and an Iron Rod.

---

## Build a network, not a cable salad

![A rail backbone along a factory floor with several lanes side by side](https://raw.githubusercontent.com/ZeeOcho/AutoConnectingPowerRails/main/docs/images/backbone.jpg)

Rails only ever connect through their terminals, and only straight on. That is the whole rule, and it is what keeps a dense factory readable: two rails side by side stay two rails, two rails end to end become one circuit. Junctions turn corners and branch; a rail dropped onto a junction's face continues from it in that face's direction.

Lanes can run 1 m apart, with junctions between them, so a four-lane power bus fits in the width of a single foundation.

A lit strip along every rail shows whether it carries power, and each terminal shows its state: open, connected, or capped.

---

## Auto-connect with blueprints

![A blueprint being placed next to an existing module; the rails at the boundary connect](https://raw.githubusercontent.com/ZeeOcho/AutoConnectingPowerRails/main/docs/images/blueprint.jpg)

Build your rails into a blueprint and the module's power comes with it. When you place that blueprint next to another one in an **Auto-Connect** build mode, open terminals at the boundary connect to their neighbours: terminals that meet flush connect directly, terminals that are set back get a short rail generated to bridge the gap. You see the connections before you click, and the price of the bridging rail is in the preview.

Where you *don't* want a connection, put a cap on the terminal. Caps are cheap, and they are also how you mark a run as finished.

---

## Plug in the rest

![An outlet on a rail with power lines to a row of machines](https://raw.githubusercontent.com/ZeeOcho/AutoConnectingPowerRails/main/docs/images/outlet.jpg)

Outlets are the only way power leaves the rail system, so you decide where the taps are. Put one on a rail's body wherever a machine row needs it, or on a free terminal at the end of a run. Each one takes four power lines, and looking at it shows how many are used, like any pole.

You don't even have to place the outlet first: start a power line at a machine and aim its other end at a rail or an open terminal, and the game builds a Power Rail Outlet there and attaches the cable — the same way it builds a Wall Outlet when you aim at a wall. Hold Ctrl to step it along the rail by the metre.

Rails and junctions also power your **Hoverpack** along their whole length, so a backbone through a build site keeps you flying.

---

## Or don't plug in at all

![A wireless outlet on a rail with link lines to several machines](https://raw.githubusercontent.com/ZeeOcho/AutoConnectingPowerRails/main/docs/images/wireless.jpg)

A **Wireless Outlet** sits on a rail like an outlet and links every machine within its range to the rail's circuit without a cable — machines, generators and power storage alike, so a generator in range feeds the network and a Power Storage in range charges from it. The range is measured to the machine's power socket, and the three tiers reach 8, 16 and 32 m.

The rules are the ones cables already follow, so nothing surprising can happen:

- A machine links to **one** outlet only, the nearest. Two outlets on separate circuits can never be bridged through a machine between them.
- Only **unwired** machines link. Attach a cable and the link drops; remove the cable and it comes back.
- A foundation, wall or roof across the line blocks it; frame pieces don't.

Each link is drawn as a line from the outlet to the machine's socket — a slow travelling bead when it carries power, a faint dashed line when it doesn't — and a linked machine wears a glowing ring round its socket. Holding a machine or wireless outlet hologram previews the links it would make, and aiming at an outlet in dismantle mode shows its range.

---

## Colour-code it

![Rails in three different swatch colours](https://raw.githubusercontent.com/ZeeOcho/AutoConnectingPowerRails/main/docs/images/swatches.jpg)

All the buildables take swatches, finishes and patterns in the Customizer, and the accent line along a rail takes the swatch's colour, so a colour-coded lane reads the same from every angle.

The swatch is the **circuit's** colour. A part that joins a network takes the network's customization as it connects — hand-built or placed from a blueprint — and a painted part keeps its own. To repaint a whole network, use the Customizer on it as you would on anything else.

---

## Good to know

- Rails are straight. For a bend, use a junction.
- A junction dropped onto a rail splits it; the rail's power lines and outlets stay where they were.
- Dismantling a rail or junction also dismantles the caps and outlets on it, with a full refund, and the preview shows what goes.
- A modded machine whose socket the glowing ring doesn't fit can be given its own fit in `Configs/AutoConnectingPowerRails_Sockets.json` beside SML's config files; `acpr.ReloadSockets` in the console applies it without a restart. The shape of the file is in the mod's `Config/GENERATED.md`.
- Single-player is tested. Multiplayer is untested in this release.

### If you remove the mod

Buildables from a removed mod disappear from the save, and a power backbone that disappears would take its factory's power with it. The mod is built so that SML's **Core Redirects** can replace every rail, junction, outlet and wireless outlet with a Wall Outlet instead — the lines are in the mod's README. With those in place your factory stays powered: the rails' connections become invisible links between the wall outlets, outlets keep their power lines, wireless links become invisible links too, caps are dropped. The wall outlets stand where the rails were, so what you lose is the look, and running cables between them is optional.

---

*Source and issues: https://github.com/ZeeOcho/AutoConnectingPowerRails*
