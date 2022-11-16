import matplotlib.pyplot as plt

def accumulate(l):
    out = [l[0]]
    for i in range(1, len(l)):
        out.append(out[-1] + l[i])

    return out

def read_file(path):
    with open(path, 'r') as f:
        lines = f.readlines()
    
    times, minor, major, cpu_use = [], [], [], []
    for line in lines[:-1]:
        t, mn, mj, c = line[:-1].split(' ')
        times.append(int(t))
        minor.append(int(mn))
        major.append(int(mj))
        cpu_use.append(int(c))

    return times, minor, major, cpu_use

def case1(path, work_nums, out_path=None):
    work_num_str = '_'.join(str(w) for w in work_nums)
    times, minor, major, _ = read_file(path)

    acc_minor = accumulate(minor)
    acc_major = accumulate(major)

    accum_flt = [a + b for a, b in zip(acc_major, acc_minor)]
    xs = [x - times[0] for x in times]

    plt.scatter(xs, accum_flt, s=3)
    plt.title(f'Accumulated Page Faults for Work Processes {work_nums[0]} and {work_nums[1]}')
    plt.ylabel('Accumulated Page Faults')
    plt.xlabel('Time (in jiffies since the start of the work processes)')
    plt.subplots_adjust(left=0.15)

    path = f'case_study_1_work_{work_num_str}.png'
    if out_path is not None:
        path = out_path
    plt.savefig(path, dpi=300)

def combined(paths, procs):
    fig, ax = plt.subplots()
    for path, proc in zip(paths, procs):
        label = f'Work Processes {proc[0]} & {proc[1]}'
        times, minor, major, _ = read_file(path)

        acc_minor = accumulate(minor)
        acc_major = accumulate(major)

        accum_flt = [a + b for a, b in zip(acc_major, acc_minor)]
        xs = [x - times[0] for x in times]

        ax.scatter(xs, accum_flt, s=3, label=label)

    plt.title(f'Accumulated Page Faults for Work Processes 1, 2, 3, and 4')
    plt.ylabel('Accumulated Page Faults')
    plt.xlabel('Time (in jiffies since the start of the work processes)')
    plt.legend(loc='best')
    plt.subplots_adjust(left=0.15)

    plt.savefig('extra/case_study_1_work_1_2_3_4.png', dpi=300)

def case2(scale=False):
    pts = []
    xs = [1, 5, 11, 16, 21]
    xaxis = [1, 2, 3, 4, 5]
    for N in xs:
        data_path = f'data/profile3_{N}.data'
        _, _, _, cpu_use = read_file(data_path)

        d = N if scale else 1
        pts.append(sum(cpu_use) / d)

    fig, ax = plt.subplots()
    ax.bar(xaxis, pts, color='r')
    ax.set_xticks(xaxis)
    ax.set_xticklabels(xs)
    plt.xlabel("Number of Work Processes")
    plt.ylabel("Total CPU Use")
    plt.title(f"Total CPU Use By Number of Processes")

    save_path = 'case_study_2_work_5.png'
    if scale:
        save_path = 'case_study_2_work_5_scaled.png'
    plt.savefig(save_path, dpi=300)

case1('data/profile1.data', [1, 2])
plt.close()
case1('data/profile2.data', [3, 4])
plt.close()
case2()
case2(scale=True)

combined(['data/profile1.data', 'data/profile2.data'], [(1, 2), (3, 4)])