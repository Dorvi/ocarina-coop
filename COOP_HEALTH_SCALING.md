# Escalado de vida en co-op / Co-op health scaling

Los jefes aguantan más golpes cuando varios jugadores pelean juntos.
Bosses take more hits when several players fight them together.

## Cómo se usa

El dueño de la sala (host) lo configura en **Room Settings → Co-op Health Scaling**.
Requiere **Enemy Sync** activado, y no se aplica en Battle Royale.

| Opción | Qué hace | Por defecto |
| --- | --- | --- |
| **Boss HP: +X% per extra player** | Vida extra de jefes por cada jugador adicional en el mismo cuarto | +100% |
| **Mini-bosses count as bosses** | Iron Knuckle, Stalfos, Dark Link, Flare Dancer, Big Octo, Dead Hand y Lizalfos/Dinolfos usan el ajuste de jefes | Activado |
| **Enemy HP: +X% per extra player** | Lo mismo para enemigos normales | Apagado |

Ejemplos con +100%: 2 jugadores = 2x golpes, 3 jugadores = 3x, 4 jugadores = 4x.
Con +50%: 2 jugadores = 1.5x, 3 jugadores = 2x.

Solo cuentan los jugadores que están en el mismo escenario y cuarto que el jefe. Si alguien
sale del cuarto o se desconecta, el jefe vuelve a recibir más daño por golpe.

## Cómo funciona

No se le sube la vida al jefe: se reduce el daño de cada golpe y la fracción sobrante se
acumula para el siguiente. Así cada fase del jefe (el desmayo de Ganon, la regla de
Volvagia de no morir con un golpe normal, etc.) ocurre exactamente con la misma vida que en
el juego original; solo cambia cuántos golpes hacen falta.

El daño se escala en `Actor_ApplyDamage` (enemigos normales, Bongo Bongo y todo golpe que
un cliente reenvía al room master) y en el punto donde cada jefe resta su vida: Gohma, King
Dodongo, Barinade, Phantom Ganon, Volvagia, Morpha, Twinrova, Ganondorf y Ganon.

Código: `soh/soh/Network/Anchor/CoopHealthScale.{h,cpp}`.

## Límites conocidos

- La primera fase de Twinrova (Koume y Kotake por separado) cuenta golpes, no vida, así que
  no se escala; la fase final sí.
- Todos en la sala deben usar esta versión. Un host sin esta función manda 0% y nadie escala.
