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
| `List` function | `mirage_write_fn(..., "List", ..., n)` + element writes | `mirage_read_fn` + element reads | ordered lists of values |
| `numericarray` | `mirage_write_numericarray` | `mirage_read_numericarray` | dense numeric arrays |
| `association/rule` | `mirage_write_association` + `mirage_write_assoc_rule` | `mirage_read_association` + `mirage_read_assoc_rule` | key/value parameter blocks |

### Common writer calls (publisher encode example)

```c
mirage_write_start(m);
mirage_write_fn(m, "List", -1, 2);

// store a pose in a sub-list
mirage_write_fn(m, "List", -1, 3);
mirage_write_f64(m, x);
mirage_write_f64(m, y);
mirage_write_f64(m, theta);

mirage_write_i64(m, (i64) timestamp_ns);
```

### Common reader calls (subscriber decode example)

```c
char name[8];
i64 nlen = 0, argc = 0;
char pose_name[8];
i64 pose_nlen = 0, pose_argc = 0;
f64 x = 0.0, y = 0.0, theta = 0.0;
i64 timestamp_i64 = 0;
u64 timestamp_ns = 0;

mirage_read_start(m);
mirage_read_fn(m, name, &nlen, sizeof(name), &argc);
name[nlen] = '\0';          // "List"

mirage_read_fn(m, pose_name, &pose_nlen, sizeof(pose_name), &pose_argc);
pose_name[pose_nlen] = '\0'; // "List"

mirage_read_f64(m, &x);
mirage_read_f64(m, &y);
mirage_read_f64(m, &theta);

mirage_read_i64(m, &timestamp_i64);
timestamp_ns = (u64) timestamp_i64;
```

### Datatype guidance

- Use `string` for human-readable text.
- Store ordered lists of values in a Mirage `List` function rather than inventing ad hoc repeated fields. The Python decoder treats a `List` function as a native Python `list`; see `../mirage/lang/python/mirage/mirage.py` for reference.
- Use `binstring` for compressed frames or custom binary payloads.
- Use `numericarray` for large numeric tensors and images.
- Keep schemas stable and versioned per topic.
