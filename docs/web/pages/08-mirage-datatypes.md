## Mirage Datatypes Used by ROS3

ROS3 payloads are Mirage-encoded message buffers. These are the common types you will see.

### Core type map

| Type | Write API | Read API | Typical ROS3 usage |
|---|---|---|---|
| `i8/i16/i32/i64` | `mirage_write_i*` | `mirage_read_i*` | counters, ids, timestamps |
| `f64` | `mirage_write_f64` | `mirage_read_f64` | pose, velocity, sensor scalars |
| `string` | `mirage_write_string` | `mirage_read_string` | text topics, names, status |
| `symbol` | `mirage_write_sym` | `mirage_read_sym` | enum-like labels |
| `binstring` | `mirage_write_bin` | `mirage_read_bin` | compressed payloads, frames |
| `function` | `mirage_write_fn` | `mirage_read_fn` | typed message envelope |
| `numericarray` | `mirage_write_numericarray` | `mirage_read_numericarray` | dense numeric arrays |
| `association/rule` | `mirage_write_association` + `mirage_write_assoc_rule` | `mirage_read_association` + `mirage_read_assoc_rule` | key/value parameter blocks |

### Common writer calls

```c
mirage_write_start(m);
mirage_write_fn(m, "Pose", -1, 3);
mirage_write_f64(m, x);
mirage_write_f64(m, y);
mirage_write_f64(m, theta);
```

### Common reader calls

```c
char name[64]; i64 nlen=0, argc=0;
mirage_read_start(m);
mirage_read_fn(m, name, &nlen, sizeof(name), &argc);
```

### Datatype guidance

- Use `string` for human-readable text.
- Use `binstring` for compressed frames or custom binary payloads.
- Use `numericarray` for large numeric tensors and images.
- Keep schemas stable and versioned per topic.
