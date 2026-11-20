# 3D printer: bindings

The examples use the entity names from the [ha-bambulab](https://github.com/greghesp/ha-bambulab) integration
(Bambu Lab printers). Other printers (OctoPrint, Moonraker, PrusaLink) work too, provided the status is mapped to
the words below.

| Value | Kind | ha-bambulab example |
|---|---|---|
| `printer.status` | text: `running`, `pause`, `prepare`, `finish`, `failed` (anything else hides the screen) | `sensor.<printer>_print_status` |
| `printer.name` | text | `sensor.<printer>_task_name` |
| `printer.progress` | number, 0-100 | `sensor.<printer>_print_progress` |
| `printer.layer` | number | `sensor.<printer>_current_layer` |
| `printer.layers` | number | `sensor.<printer>_total_layer_count` |
| `printer.remaining` | number, **minutes** | `sensor.<printer>_remaining_time` (check the unit, see below) |
| `printer.nozzle` | number | `sensor.<printer>_nozzle_temperature` |
| `printer.bed` | number | `sensor.<printer>_bed_temperature` |
| `printer.end_time` | timestamp | `sensor.<printer>_end_time` (shown as "5M AGO" after a finish or failure) |

Required: `printer.status` and `printer.progress`.

## Icons
The print head uses two small icons, `printer_head` (animated) and `printer_head_still`. They're original art in
`examples/icons/` (`make_printer_head.py` draws them). Upload both to the display's icon library. Without them the
screen still works, just without the print head.

## When it shows
- While printing, paused or preparing, if updated within the last hour.
- For 30 minutes after `finish` or `failed`, but only if the display saw the status change. A restart doesn't bring
  back an old "DONE!".

## Notes
- **Remaining time unit:** some ha-bambulab installs report `remaining_time` in hours, others in minutes. If
  yours is in hours, change the template in the screen to `{printer.remaining|dur:h}`.
- Other integrations: map their states to these words, for example OctoPrint's `Printing` to `running`.
- The tiny font only has capitals, digits and `- + . : % / *`. Other characters in the job name show as blanks.

## States to try (Sample values in the editor)
`running` (as in `3D printer.values.json`), `pause`, `prepare`, `finish`, `failed`.
